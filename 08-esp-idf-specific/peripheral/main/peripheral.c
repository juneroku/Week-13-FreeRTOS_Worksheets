// ESP32-Peripheral-Integration.c
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_netif.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"

// I2C (API ใหม่ - v5.x)
#include "driver/i2c_master.h"

// delay us จาก ROM (IDF v5.x)
#include "esp_rom_sys.h"

/* ---------------- User configuration ---------------- */
// เปิดโหมดจำลอง I2C เมื่อไม่มีอุปกรณ์จริง
#define PERIPH_I2C_MOCK      1   // 1=จำลอง, 0=ใช้อุปกรณ์จริง

#define WIFI_SSID            "Maibok"
#define WIFI_PASS            "Phumkondee2548"

#define TAG_WIFI             "WIFI"
#define TAG_PERIPH           "PERIPH"

#define CORE0                0
#define CORE1                1

/* ปุ่ม/LED (บอร์ด DevKitC ทั่วไป) */
#define GPIO_BTN             0
#define GPIO_LED             2

/* I2C pins */
#define I2C_PORT             I2C_NUM_0
#define I2C_SDA              21
#define I2C_SCL              22
#define I2C_SPEED_HZ         (50*1000)   // ช้า-เสถียร
#define I2C_ADDR_7B          0x68        // ตัวอย่าง (DS3231/MPU6050)

#define I2C_PROBE_INTERVAL_MS_OK    1000   // เมื่ออ่านได้: 1s/ครั้ง
#define I2C_PROBE_INTERVAL_MS_ERROR 3000   // เมื่อผิดพลาด: 3s/ครั้ง (ลดสแปม)
#define I2C_XFER_TIMEOUT_MS          200

/* SPI (VSPI) pins */
#define SPI_HOST_USED        SPI2_HOST
#define SPI_MOSI             23
#define SPI_MISO             19
#define SPI_SCLK             18
#define SPI_CS               5
#define SPI_CLK_HZ           (1*1000*1000)

/* GPTimer 1 kHz */
#define TIMER_HZ             1000

/* FreeRTOS prios (< configMAX_PRIORITIES=25) */
#define PRI_TIMER_WORK       14
#define PRI_WIFI_WORK        12
#define PRI_I2C_WORK        11
#define PRI_SPI_WORK        11
#define PRI_BTN_WORK        10
#define PRI_BG_WORK          5

/* ---------------- Globals ---------------- */
static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t s_btn_evt_q;
static QueueHandle_t s_timer_q;
static SemaphoreHandle_t s_io_mutex;
static spi_device_handle_t s_spi_dev;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;

/* WiFi bits */
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

typedef struct {
    uint32_t seq;
    uint8_t  last_i2c_whoami;
    uint32_t last_spi_echo;
    uint64_t last_update_us;
} shared_payload_t;

static shared_payload_t s_shared = {0};

static inline uint64_t now_us(void) { return esp_timer_get_time(); }

/* ---------------- Button ISR ---------------- */
typedef struct {
    int64_t ts_us;
    int pin;
    int level;
} btn_evt_t;

static volatile int64_t s_btn_last_us = 0;

static void IRAM_ATTR btn_isr(void *arg)
{
    int64_t t = now_us();
    if (t - s_btn_last_us < 20000) return; // debounce 20ms
    s_btn_last_us = t;

    btn_evt_t evt = {
        .ts_us = t,
        .pin   = (int)(intptr_t)arg,
        .level = gpio_get_level(GPIO_BTN)
    };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_btn_evt_q, &evt, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* ---------------- GPTimer ISR ---------------- */
static bool IRAM_ATTR gptimer_on_alarm_cb(gptimer_handle_t timer,
                                          const gptimer_alarm_event_data_t *edata,
                                          void *user_ctx)
{
    uint32_t tick = (uint32_t) edata->count_value;
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_timer_q, &tick, &hpw);
    return hpw == pdTRUE;
}

/* ---------------- WiFi ---------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_WIFI, "STA start, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG_WIFI, "Disconnected, retry...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG_WIFI, "Got IP");
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 10;
    cfg.dynamic_rx_buf_num = 32;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------- I2C helpers ---------------- */

// กู้บัส I2C แบบ manual (clock 9 ครั้ง + STOP)
static esp_err_t i2c_bus_recover(int scl_gpio, int sda_gpio)
{
    // input+pullup เพื่ออ่านเส้นก่อน
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << scl_gpio) | (1ULL << sda_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    // toggle SCL ~9 ครั้ง
    gpio_set_direction(scl_gpio, GPIO_MODE_OUTPUT_OD);
    for (int i=0; i<9; ++i) {
        gpio_set_level(scl_gpio, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl_gpio, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(sda_gpio)) break; // SDA ปล่อยแล้ว
    }
    // สร้าง STOP
    gpio_set_direction(sda_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(sda_gpio, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl_gpio, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda_gpio, 1);
    esp_rom_delay_us(5);
    return ESP_OK;
}

static esp_err_t i2c_reinit_device(void)
{
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = I2C_ADDR_7B,
        .scl_speed_hz    = I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
}

#if PERIPH_I2C_MOCK
// โหมดจำลอง: คืนค่า WHO_AM_I ปลอม เพื่อให้ flow ทั้งระบบเดินได้
static esp_err_t i2c_read_whoami_mock(uint8_t *out)
{
    static uint8_t fake = 0x68;       // ปลอมเป็นอุปกรณ์ที่ addr 0x68
    *out = fake;
    return ESP_OK;
}
#endif

// อ่าน 1 ไบต์จากรีจิสเตอร์ regAddr
static esp_err_t i2c_read_reg_1(uint8_t regAddr, uint8_t *out)
{
#if PERIPH_I2C_MOCK
    (void)regAddr;
    return i2c_read_whoami_mock(out);
#else
    return i2c_master_transmit_receive(
        s_i2c_dev,
        &regAddr, 1,
        out,      1,
        I2C_XFER_TIMEOUT_MS
    );
#endif
}

/* ---------------- I2C task ---------------- */
static void i2c_task(void *arg)
{
    ESP_LOGI(TAG_PERIPH, "I2C task start on Core %d", xPortGetCoreID());
    ESP_LOGW(TAG_PERIPH, "TIP: ถ้าจะใช้ SPI echo ให้จัมพ์ MOSI(23) ↔ MISO(19)");
#if !PERIPH_I2C_MOCK
    ESP_LOGW(TAG_PERIPH, "TIP: I2C ต้องมี pull-up 4.7k–10k ไป 3.3V (หรือเปิด internal) และ GND/3V3 ต่อครบ");
#endif

    // Master Bus init
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // เปิด internal pull-up (ช่วยได้กรณีสายสั้น)
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_ERROR_CHECK(i2c_reinit_device());

#if !PERIPH_I2C_MOCK
    // Probe รอบแรก (ไม่สแปม)
    esp_err_t pr = i2c_master_probe(s_i2c_bus, I2C_ADDR_7B, I2C_XFER_TIMEOUT_MS);
    if (pr != ESP_OK) {
        ESP_LOGW(TAG_PERIPH, "I2C 0x%02X probe fail: %s → recover (ครั้งเดียว)",
                 I2C_ADDR_7B, esp_err_to_name(pr));
        i2c_bus_recover(I2C_SCL, I2C_SDA);
        i2c_reinit_device();
    }
#endif

    const uint8_t WHO_AM_I_REG = 0x75; // ใช้เป็นตัวอย่าง reg
    uint64_t next_try_us = now_us();   // คุม rate ไม่ให้ถี่เกิน

    while (1) {
        uint64_t tnow = now_us();
        if (tnow < next_try_us) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t val = 0;
        esp_err_t e = i2c_read_reg_1(WHO_AM_I_REG, &val);

        if (e == ESP_OK) {
            xSemaphoreTake(s_io_mutex, portMAX_DELAY);
            s_shared.seq++;
            s_shared.last_i2c_whoami = val;
            s_shared.last_update_us = now_us();
            xSemaphoreGive(s_io_mutex);
            ESP_LOGI(TAG_PERIPH, "I2C WHO_AM_I=0x%02X", val);

            // อ่านสำเร็จ → รอบหน้า 1 วินาที
            next_try_us = now_us() + (uint64_t)I2C_PROBE_INTERVAL_MS_OK * 1000ULL;
        } else {
            // ลด log เป็น WARN และรีคัฟเวอร์แบบคั่นจังหวะ (3s)
            ESP_LOGW(TAG_PERIPH, "I2C xfer error: %s (ลดความถี่การ recover/log)", esp_err_to_name(e));

#if !PERIPH_I2C_MOCK
            i2c_bus_recover(I2C_SCL, I2C_SDA);

            // รีแอดดีไวซ์แก้ INVALID_STATE
            if (i2c_reinit_device() != ESP_OK) {
                ESP_LOGW(TAG_PERIPH, "Re-add I2C dev failed, re-init bus");
                i2c_del_master_bus(s_i2c_bus);
                s_i2c_bus = NULL;
                ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
                ESP_ERROR_CHECK(i2c_reinit_device());
            }
            // ไม่ probe ถี่เกิน—เว้นช่วงแล้วค่อยลองใหม่
#endif
            next_try_us = now_us() + (uint64_t)I2C_PROBE_INTERVAL_MS_ERROR * 1000ULL;
        }

        // กัน while กิน CPU
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ---------------- SPI Master ---------------- */
static void spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI,
        .miso_io_num = SPI_MISO,
        .sclk_io_num = SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_USED, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLK_HZ,
        .mode = 0,
        .spics_io_num = SPI_CS,
        .queue_size = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST_USED, &devcfg, &s_spi_dev));
}

static void spi_task(void *arg)
{
    ESP_LOGI(TAG_PERIPH, "SPI task start on Core %d", xPortGetCoreID());
    ESP_LOGW(TAG_PERIPH, "TIP: ถ้าจะทดสอบ echo ให้จัมพ์ MOSI(23) ↔ MISO(19)");
    spi_init();

    uint32_t counter = 0;
    while (1) {
        uint32_t tx = counter++;
        uint32_t rx = 0;
        spi_transaction_t t = {
            .length = 8 * sizeof(tx),
            .tx_buffer = &tx,
            .rx_buffer = &rx
        };
        esp_err_t e = spi_device_transmit(s_spi_dev, &t);
        if (e == ESP_OK) {
            xSemaphoreTake(s_io_mutex, portMAX_DELAY);
            s_shared.seq++;
            s_shared.last_spi_echo = rx;
            s_shared.last_update_us = now_us();
            xSemaphoreGive(s_io_mutex);
            ESP_LOGI(TAG_PERIPH, "SPI tx=0x%08" PRIx32 " rx=0x%08" PRIx32, tx, rx);
        } else {
            ESP_LOGW(TAG_PERIPH, "SPI transmit fail: %s", esp_err_to_name(e));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------- GPTimer worker ------------- */
static void timer_worker_task(void *arg)
{
    ESP_LOGI(TAG_PERIPH, "Timer worker start on Core %d", xPortGetCoreID());

    gptimer_handle_t timer = NULL;
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &timer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = gptimer_on_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));

    gptimer_alarm_config_t alarm = {
        .reload_count = 0,
        .alarm_count = 1000000 / TIMER_HZ,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));

    uint32_t tick;
    uint32_t acc = 0;
    uint64_t t0 = now_us();
    while (1) {
        if (xQueueReceive(s_timer_q, &tick, portMAX_DELAY) == pdTRUE) {
            acc++;
            if (acc % 1000 == 0) {
                uint64_t t1 = now_us();
                double hz = (double)acc * 1e6 / (double)(t1 - t0);
                ESP_LOGI(TAG_PERIPH, "Timer rate ~ %.1f Hz", hz);
                acc = 0;
                t0 = t1;
            }
        }
    }
}

/* ------------- Button worker ------------- */
static void button_task(void *arg)
{
    ESP_LOGI(TAG_PERIPH, "Button task start on Core %d", xPortGetCoreID());
    btn_evt_t evt;

    gpio_config_t io_btn = {
        .pin_bit_mask = 1ULL<<GPIO_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io_btn);

    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(GPIO_BTN, btn_isr, (void*)(intptr_t)GPIO_BTN);

    gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);

    while (1) {
        if (xQueueReceive(s_btn_evt_q, &evt, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG_PERIPH, "Button evt: pin=%d level=%d ts=%" PRId64 " us",
                     evt.pin, evt.level, evt.ts_us);
            gpio_set_level(GPIO_LED, evt.level ? 1 : 0);
        }
    }
}

/* ------------- WiFi worker ------------- */
static void wifi_worker_task(void *arg)
{
    ESP_LOGI(TAG_PERIPH, "WiFi worker start on Core %d", xPortGetCoreID());
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG_PERIPH, "WiFi connected, doing periodic network work...");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ------------- Background ------------- */
static void background_task(void *arg)
{
    while (1) {
        size_t free_heap = esp_get_free_heap_size();
        shared_payload_t snap;
        xSemaphoreTake(s_io_mutex, portMAX_DELAY);
        snap = s_shared;
        xSemaphoreGive(s_io_mutex);

        ESP_LOGI(TAG_PERIPH, "Free heap: %u bytes | seq=%" PRIu32
                             " i2c_whoami=0x%02X spi_echo=0x%08" PRIx32
                             " updated=%" PRIu64 " us",
                 (unsigned)free_heap, snap.seq, snap.last_i2c_whoami,
                 snap.last_spi_echo, snap.last_update_us);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ------------------- app_main ------------------- */
void app_main(void)
{
    ESP_LOGI(TAG_PERIPH, "Peripheral Integration Demo; Main on Core %d", xPortGetCoreID());
    ESP_ERROR_CHECK(nvs_flash_init());

    s_btn_evt_q = xQueueCreate(16, sizeof(btn_evt_t));
    s_timer_q   = xQueueCreate(32, sizeof(uint32_t));
    s_io_mutex  = xSemaphoreCreateMutex();

    wifi_init_sta();

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(timer_worker_task, "TIMER", 4096, NULL, PRI_TIMER_WORK, NULL, CORE0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(button_task, "BUTTON", 4096, NULL, PRI_BTN_WORK, NULL, CORE0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(wifi_worker_task, "WIFI_WRK", 4096, NULL, PRI_WIFI_WORK, NULL, CORE1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(i2c_task, "I2C_NEW", 4096, NULL, PRI_I2C_WORK, NULL, CORE1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(spi_task, "SPI", 4096, NULL, PRI_SPI_WORK, NULL, CORE1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreate(background_task, "BG", 4096, NULL, PRI_BG_WORK, NULL);
    configASSERT(ok == pdPASS);

    ESP_LOGI(TAG_PERIPH, "System started: WiFi + Timer + GPIO ISR + I2C + SPI, shared safely across cores.");
}