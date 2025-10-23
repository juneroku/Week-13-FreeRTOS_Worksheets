#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "MEM_POOLS";

// GPIO สำหรับแสดงสถานะ pool
#define LED_SMALL_POOL     GPIO_NUM_2   // Small pool activity
#define LED_MEDIUM_POOL    GPIO_NUM_4   // Medium pool activity
#define LED_LARGE_POOL     GPIO_NUM_5   // Large pool activity
#define LED_POOL_FULL      GPIO_NUM_18  // Pool exhaustion
#define LED_POOL_ERROR     GPIO_NUM_19  // Pool error/corruption

// Memory pool configurations
#define SMALL_POOL_BLOCK_SIZE   64
#define SMALL_POOL_BLOCK_COUNT  32

#define MEDIUM_POOL_BLOCK_SIZE  256
#define MEDIUM_POOL_BLOCK_COUNT 16

#define LARGE_POOL_BLOCK_SIZE   1024
#define LARGE_POOL_BLOCK_COUNT  8

#define HUGE_POOL_BLOCK_SIZE    4096
#define HUGE_POOL_BLOCK_COUNT   4

// ====== Pool management structures ======
typedef struct memory_block {
    struct memory_block* next;
    uint32_t magic;        // For corruption detection
    uint32_t pool_id;      // Which pool this block belongs to
    uint64_t alloc_time;   // When was this allocated
} memory_block_t;

typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    size_t alignment;
    uint32_t caps;

    // Pool memory
    void* pool_memory;
    memory_block_t* free_list;
    uint8_t* usage_bitmap;  // byte-accurate bitmap (1 bit/block)

    // Statistics
    size_t allocated_blocks;
    size_t peak_usage;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
    uint32_t allocation_failures;

    // Synchronization
    SemaphoreHandle_t mutex;

    // Pool ID for corruption detection
    uint32_t pool_id;
} memory_pool_t;

typedef enum {
    POOL_SMALL = 0,
    POOL_MEDIUM,
    POOL_LARGE,
    POOL_HUGE,
    POOL_COUNT
} pool_type_t;

static memory_pool_t pools[POOL_COUNT];
static bool pools_initialized = false;

// คอนฟิกพูล (Huge ขอ SPIRAM ก่อน ถ้าไม่มีจะ fallback อัตโนมัติใน init)
typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    uint32_t caps;
    gpio_num_t led_pin;
} pool_config_t;

static const pool_config_t pool_configs[POOL_COUNT] = {
    {"Small",  SMALL_POOL_BLOCK_SIZE,  SMALL_POOL_BLOCK_COUNT,  MALLOC_CAP_INTERNAL,                    LED_SMALL_POOL},
    {"Medium", MEDIUM_POOL_BLOCK_SIZE, MEDIUM_POOL_BLOCK_COUNT, MALLOC_CAP_INTERNAL,                    LED_MEDIUM_POOL},
    {"Large",  LARGE_POOL_BLOCK_SIZE,  LARGE_POOL_BLOCK_COUNT,  MALLOC_CAP_DEFAULT,                     LED_LARGE_POOL},
    {"Huge",   HUGE_POOL_BLOCK_SIZE,   HUGE_POOL_BLOCK_COUNT,   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),  LED_POOL_FULL}
};

// Magic numbers
#define POOL_MAGIC_FREE    0xDEADBEEF
#define POOL_MAGIC_ALLOC   0xCAFEBABE

// ====== Pool management ======
static inline size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

bool init_memory_pool(memory_pool_t* pool, const pool_config_t* config, uint32_t pool_id) {
    if (!pool || !config) return false;

    memset(pool, 0, sizeof(memory_pool_t));
    pool->name        = config->name;
    pool->block_size  = config->block_size;
    pool->block_count = config->block_count;
    pool->alignment   = 4; // 4-byte alignment
    pool->caps        = config->caps;
    pool->pool_id     = pool_id;

    // คำนวณขนาดจริงต่อบล็อก
    const size_t header_size        = sizeof(memory_block_t);
    const size_t aligned_block_size = align_up(config->block_size, pool->alignment);
    const size_t total_block_size   = header_size + aligned_block_size;
    const size_t total_memory       = total_block_size * config->block_count;

    // ขอ 8-bit capable เสมอ และทำ fallback ถ้าขอ SPIRAM แต่ไม่มี
    uint32_t req_caps = (config->caps | MALLOC_CAP_8BIT);
    if ((req_caps & MALLOC_CAP_SPIRAM) && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
        ESP_LOGW(TAG, "%s pool requested SPIRAM but none available. Falling back to INTERNAL DRAM.", config->name);
        req_caps = (req_caps & ~MALLOC_CAP_SPIRAM) | MALLOC_CAP_INTERNAL;
    }

    pool->pool_memory = heap_caps_malloc(total_memory, req_caps);
    if (!pool->pool_memory) {
        ESP_LOGE(TAG, "Failed to allocate memory for %s pool", config->name);
        return false;
    }

    // Bitmap (1 bit/block) อยู่ใน INTERNAL 8-bit
    const size_t bitmap_bytes = (config->block_count + 7) / 8;
    pool->usage_bitmap = (uint8_t*)heap_caps_calloc(bitmap_bytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pool->usage_bitmap) {
        heap_caps_free(pool->pool_memory);
        ESP_LOGE(TAG, "Failed to allocate bitmap for %s pool", config->name);
        return false;
    }

    // สร้าง free list
    uint8_t* memory_ptr = (uint8_t*)pool->pool_memory;
    pool->free_list = NULL;
    for (int i = 0; i < (int)config->block_count; i++) {
        memory_block_t* block = (memory_block_t*)(memory_ptr + (i * total_block_size));
        block->magic = POOL_MAGIC_FREE;
        block->pool_id = pool_id;
        block->alloc_time = 0;
        block->next = pool->free_list;
        pool->free_list = block;
    }

    // Mutex
    pool->mutex = xSemaphoreCreateMutex();
    if (!pool->mutex) {
        heap_caps_free(pool->pool_memory);
        heap_caps_free(pool->usage_bitmap);
        ESP_LOGE(TAG, "Failed to create mutex for %s pool", config->name);
        return false;
    }

    ESP_LOGI(TAG, "✅ Initialized %s pool: %d blocks × %d bytes = %d total bytes",
             config->name, (int)config->block_count, (int)config->block_size, (int)total_memory);
    return true;
}

void* pool_malloc(memory_pool_t* pool) {
    if (!pool || !pool->mutex) return NULL;

    uint64_t start_time = esp_timer_get_time();
    void* result = NULL;

    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (pool->free_list) {
            memory_block_t* block = pool->free_list;
            pool->free_list = block->next;

            if (block->magic != POOL_MAGIC_FREE || block->pool_id != pool->pool_id) {
                ESP_LOGE(TAG, "🚨 Corruption detected in %s pool block %p!", pool->name, block);
                gpio_set_level(LED_POOL_ERROR, 1);
                xSemaphoreGive(pool->mutex);
                return NULL;
            }

            block->magic = POOL_MAGIC_ALLOC;
            block->alloc_time = esp_timer_get_time();
            block->next = NULL;

            pool->allocated_blocks++;
            if (pool->allocated_blocks > pool->peak_usage) pool->peak_usage = pool->allocated_blocks;
            pool->total_allocations++;

            const size_t header_size        = sizeof(memory_block_t);
            const size_t aligned_block_size = align_up(pool->block_size, pool->alignment);
            const size_t total_block_size   = header_size + aligned_block_size;
            const size_t block_index = ((uint8_t*)block - (uint8_t*)pool->pool_memory) / total_block_size;

            if (block_index < pool->block_count) {
                pool->usage_bitmap[block_index / 8] |= (1U << (block_index % 8));
            }

            result = (uint8_t*)block + header_size;
            ESP_LOGD(TAG, "🟢 %s pool: allocated block %p (index %d)", pool->name, result, (int)block_index);
        } else {
            pool->allocation_failures++;
            ESP_LOGW(TAG, "🔴 %s pool exhausted! (%d/%d blocks used)", pool->name, (int)pool->allocated_blocks, (int)pool->block_count);
            gpio_set_level(LED_POOL_FULL, 1);
        }
        xSemaphoreGive(pool->mutex);
    }

    pool->allocation_time_total += (esp_timer_get_time() - start_time);
    return result;
}

bool pool_free(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr || !pool->mutex) return false;

    uint64_t start_time = esp_timer_get_time();
    bool ok = false;

    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const size_t header_size = sizeof(memory_block_t);
        memory_block_t* block = (memory_block_t*)((uint8_t*)ptr - header_size);

        if (block->magic != POOL_MAGIC_ALLOC || block->pool_id != pool->pool_id) {
            ESP_LOGE(TAG, "🚨 Invalid block %p for %s pool! Magic: 0x%08X, Pool ID: %lu",
                     ptr, pool->name, block->magic, (unsigned long)block->pool_id);
            gpio_set_level(LED_POOL_ERROR, 1);
            xSemaphoreGive(pool->mutex);
            return false;
        }

        const size_t aligned_block_size = align_up(pool->block_size, pool->alignment);
        const size_t total_block_size   = header_size + aligned_block_size;

        if ((uint8_t*)block >= (uint8_t*)pool->pool_memory &&
            (uint8_t*)block < (uint8_t*)pool->pool_memory + (total_block_size * pool->block_count)) {

            const size_t block_index = ((uint8_t*)block - (uint8_t*)pool->pool_memory) / total_block_size;

            if (block_index < pool->block_count) {
                pool->usage_bitmap[block_index / 8] &= ~(1U << (block_index % 8));
            }

            block->magic = POOL_MAGIC_FREE;
            block->next = pool->free_list;
            pool->free_list = block;

            pool->allocated_blocks--;
            pool->total_deallocations++;
            ok = true;

            ESP_LOGD(TAG, "🟢 %s pool: freed block %p (index %d)", pool->name, ptr, (int)block_index);
        } else {
            ESP_LOGE(TAG, "🚨 Block %p out of bounds for %s pool!", ptr, pool->name);
            gpio_set_level(LED_POOL_ERROR, 1);
        }
        xSemaphoreGive(pool->mutex);
    }

    pool->deallocation_time_total += (esp_timer_get_time() - start_time);
    return ok;
}

// ====== Smart pool allocator ======
void* smart_pool_malloc(size_t size) {
    const size_t required_size = size + 16; // safety margin
    for (int i = 0; i < POOL_COUNT; i++) {
        if (required_size <= pools[i].block_size) {
            void* ptr = pool_malloc(&pools[i]);
            if (ptr) {
                gpio_set_level(pool_configs[i].led_pin, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(pool_configs[i].led_pin, 0);
                ESP_LOGD(TAG, "🎯 Smart allocation: %d bytes from %s pool", (int)size, pools[i].name);
                return ptr;
            }
        }
    }
    ESP_LOGW(TAG, "⚠️ No suitable pool for %d bytes, falling back to heap", (int)size);
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
}

bool smart_pool_free(void* ptr) {
    if (!ptr) return false;
    for (int i = 0; i < POOL_COUNT; i++) {
        if (pool_free(&pools[i], ptr)) return true;
    }
    ESP_LOGD(TAG, "🎯 Freeing %p from heap (not from pool)", ptr);
    heap_caps_free(ptr);
    return true;
}

// ====== Monitoring / Stats ======
void print_pool_statistics(void) {
    ESP_LOGI(TAG, "\n📊 ═══ MEMORY POOL STATISTICS ═══");
    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* pool = &pools[i];
        if (pool->mutex && xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "\n%s Pool:", pool->name);
            ESP_LOGI(TAG, "  Block Size:      %d bytes", (int)pool->block_size);
            ESP_LOGI(TAG, "  Total Blocks:    %d", (int)pool->block_count);
            ESP_LOGI(TAG, "  Used Blocks:     %d (%d%%)",
                     (int)pool->allocated_blocks,
                     (int)((pool->allocated_blocks * 100) / pool->block_count));
            ESP_LOGI(TAG, "  Peak Usage:      %d blocks", (int)pool->peak_usage);
            ESP_LOGI(TAG, "  Allocations:     %llu", pool->total_allocations);
            ESP_LOGI(TAG, "  Deallocations:   %llu", pool->total_deallocations);
            ESP_LOGI(TAG, "  Failures:        %lu", (unsigned long)pool->allocation_failures);
            if (pool->total_allocations > 0) {
                uint32_t avg_alloc_time = pool->allocation_time_total / pool->total_allocations;
                ESP_LOGI(TAG, "  Avg Alloc Time:  %lu μs", (unsigned long)avg_alloc_time);
            }
            if (pool->total_deallocations > 0) {
                uint32_t avg_dealloc_time = pool->deallocation_time_total / pool->total_deallocations;
                ESP_LOGI(TAG, "  Avg Dealloc Time: %lu μs", (unsigned long)avg_dealloc_time);
            }
            xSemaphoreGive(pool->mutex);
        }
    }
    ESP_LOGI(TAG, "═══════════════════════════════════════");
}

void visualize_pool_usage(void) {
    ESP_LOGI(TAG, "\n🎨 ═══ POOL USAGE VISUALIZATION ═══");
    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* pool = &pools[i];
        if (pool->mutex && xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            char usage_bar[33] = {0}; // 32 chars + null
            const int bar_length = 32;
            const int used_chars = (pool->allocated_blocks * bar_length) / pool->block_count;
            for (int j = 0; j < bar_length; j++) usage_bar[j] = (j < used_chars) ? '█' : '░';
            ESP_LOGI(TAG, "%s: [%s] %d/%d",
                     pool->name, usage_bar, (int)pool->allocated_blocks, (int)pool->block_count);
            xSemaphoreGive(pool->mutex);
        }
    }
    ESP_LOGI(TAG, "═══════════════════════════════════════");
}

bool check_pool_integrity(void) {
    bool all_ok = true;
    ESP_LOGI(TAG, "\n🔍 ═══ POOL INTEGRITY CHECK ═══");
    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* pool = &pools[i];
        bool pool_ok = true;
        if (pool->mutex && xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            memory_block_t* current = pool->free_list;
            int free_count = 0;
            while (current && free_count < pool->block_count) {
                if (current->magic != POOL_MAGIC_FREE || current->pool_id != pool->pool_id) {
                    ESP_LOGE(TAG, "❌ %s pool: Corrupted free block %p", pool->name, current);
                    pool_ok = false; break;
                }
                current = current->next;
                free_count++;
            }
            if (pool_ok) ESP_LOGI(TAG, "✅ %s pool: %d free blocks verified", pool->name, free_count);
            xSemaphoreGive(pool->mutex);
        }
        if (!pool_ok) { all_ok = false; gpio_set_level(LED_POOL_ERROR, 1); }
    }
    if (all_ok) { ESP_LOGI(TAG, "✅ All pools passed integrity check"); gpio_set_level(LED_POOL_ERROR, 0); }
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    return all_ok;
}

// ====== Test tasks ======
void pool_stress_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏋️ Pool stress test started");
    void*  test_ptrs[100]  = {NULL};
    size_t test_sizes[100] = {0};
    int allocation_count = 0;

    while (1) {
        int action = esp_random() % 3;

        if (action == 0 && allocation_count < 100) {
            size_t size = 16 + (esp_random() % 2048); // 16-2064 bytes
            test_ptrs[allocation_count] = smart_pool_malloc(size);
            if (test_ptrs[allocation_count]) {
                test_sizes[allocation_count] = size;
                memset(test_ptrs[allocation_count], 0xAA, size);
                allocation_count++;
                ESP_LOGI(TAG, "🏋️ Allocated %d bytes (%d/100)", (int)size, allocation_count);
            }

        } else if (action == 1 && allocation_count > 0) {
            int index = esp_random() % allocation_count;
            if (test_ptrs[index]) {
                uint8_t* data = (uint8_t*)test_ptrs[index];
                bool pattern_ok = true;
                for (size_t i = 0; i < test_sizes[index]; i++) { if (data[i] != 0xAA) { pattern_ok = false; break; } }
                if (!pattern_ok) { ESP_LOGE(TAG, "🚨 Data corruption detected in allocation %d!", index); gpio_set_level(LED_POOL_ERROR, 1); }
                smart_pool_free(test_ptrs[index]);
                for (int i = index; i < allocation_count - 1; i++) { test_ptrs[i] = test_ptrs[i + 1]; test_sizes[i] = test_sizes[i + 1]; }
                allocation_count--;
                ESP_LOGI(TAG, "🗑️ Freed allocation (%d/100)", allocation_count);
            }

        } else if (action == 2) {
            print_pool_statistics();
            visualize_pool_usage();
        }

        vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 1000))); // 0.5-1.5 s
    }
}

void pool_performance_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "⚡ Pool performance test started");
    const int test_iterations = 1000;
    const size_t test_sizes[] = {32, 128, 512, 2048};
    const int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    while (1) {
        ESP_LOGI(TAG, "\n⚡ Running performance benchmark...");
        for (int size_idx = 0; size_idx < num_sizes; size_idx++) {
            size_t test_size = test_sizes[size_idx];

            // Pool allocs
            uint64_t pool_start = esp_timer_get_time();
            void* pool_ptrs[test_iterations];
            for (int i = 0; i < test_iterations; i++) { pool_ptrs[i] = smart_pool_malloc(test_size); }
            uint64_t pool_alloc_time = esp_timer_get_time() - pool_start;

            uint64_t pool_free_start = esp_timer_get_time();
            for (int i = 0; i < test_iterations; i++) { if (pool_ptrs[i]) smart_pool_free(pool_ptrs[i]); }
            uint64_t pool_free_time = esp_timer_get_time() - pool_free_start;

            // Heap allocs
            uint64_t heap_start = esp_timer_get_time();
            void* heap_ptrs[test_iterations];
            for (int i = 0; i < test_iterations; i++) { heap_ptrs[i] = heap_caps_malloc(test_size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT); }
            uint64_t heap_alloc_time = esp_timer_get_time() - heap_start;

            uint64_t heap_free_start = esp_timer_get_time();
            for (int i = 0; i < test_iterations; i++) { if (heap_ptrs[i]) heap_caps_free(heap_ptrs[i]); }
            uint64_t heap_free_time = esp_timer_get_time() - heap_free_start;

            ESP_LOGI(TAG, "\n📏 Size: %d bytes (%d iterations)", (int)test_size, test_iterations);
            ESP_LOGI(TAG, "Pool Alloc:  %llu μs (%.2f μs/alloc)", pool_alloc_time, (float)pool_alloc_time / test_iterations);
            ESP_LOGI(TAG, "Pool Free:   %llu μs (%.2f μs/free)",  pool_free_time, (float)pool_free_time / test_iterations);
            ESP_LOGI(TAG, "Heap Alloc:  %llu μs (%.2f μs/alloc)", heap_alloc_time, (float)heap_alloc_time / test_iterations);
            ESP_LOGI(TAG, "Heap Free:   %llu μs (%.2f μs/free)",  heap_free_time, (float)heap_free_time / test_iterations);

            float alloc_speedup = (float)heap_alloc_time / (float)pool_alloc_time;
            float free_speedup  = (float)heap_free_time  / (float)pool_free_time;
            ESP_LOGI(TAG, "Speedup: Alloc %.2fx, Free %.2fx", alloc_speedup, free_speedup);
        }
        vTaskDelay(pdMS_TO_TICKS(30000)); // 30 s
    }
}

void pool_pattern_test_task(void *pvParameters) {
    ESP_LOGI(TAG, "🎨 Pool pattern test started");
    typedef struct { uint32_t pattern; size_t size; void* ptr; } pattern_test_t;
    pattern_test_t tests[50];
    int test_count = 0;

    while (1) {
        ESP_LOGI(TAG, "🎨 Creating pattern allocations...");
        for (int i = 0; i < 50; i++) {
            size_t size = 32 + (esp_random() % 1000); // 32-1032 bytes
            uint32_t pattern = esp_random();
            tests[i].ptr = smart_pool_malloc(size);
            if (tests[i].ptr) {
                tests[i].size = size;
                tests[i].pattern = pattern;
                uint32_t* data = (uint32_t*)tests[i].ptr;
                size_t words = size / sizeof(uint32_t);
                for (size_t j = 0; j < words; j++) data[j] = pattern;
                test_count++;
            }
        }
        ESP_LOGI(TAG, "🎨 Created %d pattern allocations", test_count);

        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_LOGI(TAG, "🎨 Verifying patterns...");
        int corruptions = 0;
        for (int i = 0; i < test_count; i++) {
            if (tests[i].ptr) {
                uint32_t* data = (uint32_t*)tests[i].ptr;
                size_t words = tests[i].size / sizeof(uint32_t);
                bool pattern_ok = true;
                for (size_t j = 0; j < words; j++) { if (data[j] != tests[i].pattern) { pattern_ok = false; break; } }
                if (!pattern_ok) { corruptions++; ESP_LOGE(TAG, "🚨 Pattern corruption in allocation %d!", i); }
            }
        }
        if (corruptions > 0) { ESP_LOGW(TAG, "🎨 Found %d corrupted patterns", corruptions); gpio_set_level(LED_POOL_ERROR, 1); }
        else { ESP_LOGI(TAG, "🎨 All patterns verified successfully"); gpio_set_level(LED_POOL_ERROR, 0); }

        for (int i = 0; i < test_count; i++) { if (tests[i].ptr) { smart_pool_free(tests[i].ptr); tests[i].ptr = NULL; } }
        test_count = 0;
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 s
    }
}

void pool_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "📊 Pool monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // Monitor every 15 seconds
        print_pool_statistics();
        visualize_pool_usage();
        check_pool_integrity();
        bool any_exhausted = false;
        for (int i = 0; i < POOL_COUNT; i++) {
            if (pools[i].allocated_blocks >= pools[i].block_count) { any_exhausted = true; break; }
        }
        gpio_set_level(LED_POOL_FULL, any_exhausted ? 1 : 0);
        ESP_LOGI(TAG, "System uptime: %llu ms", esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "Free heap: %d bytes\n", esp_get_free_heap_size());
    }
}

// ====== app_main ======
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Memory Pools Lab Starting...");

    // GPIO
    gpio_set_direction(LED_SMALL_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEDIUM_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LARGE_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_FULL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_ERROR, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_SMALL_POOL, 0);
    gpio_set_level(LED_MEDIUM_POOL, 0);
    gpio_set_level(LED_LARGE_POOL, 0);
    gpio_set_level(LED_POOL_FULL, 0);
    gpio_set_level(LED_POOL_ERROR, 0);

    // Init pools (ไม่หยุดทั้งโปรแกรมถ้าบางพูลล้มเหลว)
    ESP_LOGI(TAG, "Initializing memory pools...");
    int ok_count = 0;
    for (int i = 0; i < POOL_COUNT; i++) {
        if (init_memory_pool(&pools[i], &pool_configs[i], i + 1)) ok_count++;
        else ESP_LOGW(TAG, "Skip %s pool (init failed).", pool_configs[i].name);
    }
    if (ok_count == 0) {
        ESP_LOGE(TAG, "No pools initialized. Exiting.");
        return;
    }

    pools_initialized = true;
    ESP_LOGI(TAG, "Initialized %d/%d pools successfully", ok_count, POOL_COUNT);

    print_pool_statistics();

    // Tasks
    ESP_LOGI(TAG, "Creating memory pool test tasks...");
    xTaskCreate(pool_monitor_task,         "PoolMonitor", 4096, NULL, 6, NULL);
    xTaskCreate(pool_stress_test_task,     "StressTest",  3072, NULL, 5, NULL);
    xTaskCreate(pool_performance_test_task,"PerfTest",    3072, NULL, 4, NULL);
    xTaskCreate(pool_pattern_test_task,    "PatternTest", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "All tasks created successfully");

    ESP_LOGI(TAG, "\n🎯 LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Small Pool Activity (64B)");
    ESP_LOGI(TAG, "  GPIO4  - Medium Pool Activity (256B)");
    ESP_LOGI(TAG, "  GPIO5  - Large Pool Activity (1KB)");
    ESP_LOGI(TAG, "  GPIO18 - Pool Full Warning");
    ESP_LOGI(TAG, "  GPIO19 - Pool Error/Corruption");

    ESP_LOGI(TAG, "\n🏊 Pool Configuration:");
    ESP_LOGI(TAG, "  Small Pool:  %d × %d bytes = %d KB", SMALL_POOL_BLOCK_COUNT, SMALL_POOL_BLOCK_SIZE,  (SMALL_POOL_BLOCK_COUNT * SMALL_POOL_BLOCK_SIZE) / 1024);
    ESP_LOGI(TAG, "  Medium Pool: %d × %d bytes = %d KB", MEDIUM_POOL_BLOCK_COUNT, MEDIUM_POOL_BLOCK_SIZE, (MEDIUM_POOL_BLOCK_COUNT * MEDIUM_POOL_BLOCK_SIZE) / 1024);
    ESP_LOGI(TAG, "  Large Pool:  %d × %d bytes = %d KB", LARGE_POOL_BLOCK_COUNT,  LARGE_POOL_BLOCK_SIZE,  (LARGE_POOL_BLOCK_COUNT  * LARGE_POOL_BLOCK_SIZE)  / 1024);
    ESP_LOGI(TAG, "  Huge Pool:   %d × %d bytes = %d KB", HUGE_POOL_BLOCK_COUNT,   HUGE_POOL_BLOCK_SIZE,   (HUGE_POOL_BLOCK_COUNT   * HUGE_POOL_BLOCK_SIZE)   / 1024);

    ESP_LOGI(TAG, "\n🧪 Test Features:");
    ESP_LOGI(TAG, "  • Multi-tier Memory Pool System");
    ESP_LOGI(TAG, "  • Smart Pool Selection");
    ESP_LOGI(TAG, "  • Performance Benchmarking");
    ESP_LOGI(TAG, "  • Corruption Detection");
    ESP_LOGI(TAG, "  • Usage Visualization");
    ESP_LOGI(TAG, "  • Integrity Checking");

    ESP_LOGI(TAG, "Memory Pool System operational!");
}