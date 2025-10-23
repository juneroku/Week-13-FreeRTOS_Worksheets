#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "REALTIME";

/* ===================== Config ====================== */
#define CORE0              0
#define CORE1              1

// ความถี่เป้าหมาย
#define CTRL_HZ            1000   // 1 kHz
#define DAQ_HZ             500    // 500 Hz

// คาบเวลา (ไมโครวินาที)
#define CTRL_PERIOD_US     (1000000 / CTRL_HZ)  // 1000 us
#define DAQ_PERIOD_US      (1000000 / DAQ_HZ)   // 2000 us

// ลำดับความสำคัญ (ต้อง < configMAX_PRIORITIES = 25)
#define PRIO_CTRL          24
#define PRIO_DAQ           22
#define PRIO_COMM          18
#define PRIO_BG             5

// สแต็ก
#define STK_CTRL           4096
#define STK_DAQ            4096
#define STK_COMM           4096
#define STK_BG             4096

// ช่วงเวลารายงานผล (มิลลิวินาที)
#define REPORT_MS          1000

/* ============= โครงสร้าง/คิวสำหรับสื่อสาร ============ */
typedef struct {
    int64_t t_send_us;      // เวลาส่ง (us)
    uint32_t seq;
    float ctrl_output;      // ผลลัพธ์จาก control loop (ตัวอย่าง)
} ctrl_msg_t;

static QueueHandle_t q_ctrl_to_comm;

/* ============= ตัวช่วยวัดความถี่/จิตเตอร์ ============= */
typedef struct {
    int64_t prev_tick_us;
    int64_t target_period_us;
    // สถิติอย่างง่าย
    double err_abs_sum_us;
    double err_abs_max_us;
    uint32_t count;
} period_stats_t;

static inline void stats_init(period_stats_t *s, int64_t period_us) {
    memset(s, 0, sizeof(*s));
    s->target_period_us = period_us;
    s->prev_tick_us = 0;
    s->err_abs_sum_us = 0.0;
    s->err_abs_max_us = 0.0;
    s->count = 0;
}

static inline void stats_update(period_stats_t *s, int64_t now_us) {
    if (s->prev_tick_us == 0) {
        s->prev_tick_us = now_us;
        return;
    }
    int64_t dt = now_us - s->prev_tick_us;
    s->prev_tick_us = now_us;

    double err = (double)dt - (double)s->target_period_us;   // us
    double aerr = fabs(err);
    s->err_abs_sum_us += aerr;
    if (aerr > s->err_abs_max_us) s->err_abs_max_us = aerr;
    s->count++;
}

static inline void stats_report_and_clear(const char *tag_name, const char *label, const period_stats_t *s) {
    if (s->count == 0) return;
    double avg_abs_err = s->err_abs_sum_us / (double)s->count;   // us
    double jitter_pct  = (avg_abs_err / (double)s->target_period_us) * 100.0; // %
    double max_jitter_pct = (s->err_abs_max_us / (double)s->target_period_us) * 100.0;

    double hz = 1e6 / (double)s->target_period_us;
    ESP_LOGI(tag_name, "%s: %.1f Hz (jitter avg: ±%.2f%%, max: ±%.2f%%)",
             label, hz, jitter_pct, max_jitter_pct);
}

/* ======= vTaskDelayUntil แบบความละเอียด us บน esp_timer ======= */
/* ใช้ clock ความละเอียดสูงเพื่อคุม 1kHz/500Hz แบบเนียน ไม่ busy-wait ยาว */
static inline void delay_until_us(int64_t *next_deadline_us, int64_t period_us)
{
    int64_t now = esp_timer_get_time();
    if (*next_deadline_us == 0) {
        *next_deadline_us = now + period_us;
    } else {
        *next_deadline_us += period_us;
    }
    int64_t wait_us = *next_deadline_us - now;
    if (wait_us <= 0) {
        // ถ้าช้ากว่ากำหนด ให้ตามให้ทัน แต่ไม่ busy-wait
        return;
    }
    // แปลงเป็น ticks แล้วใช้ vTaskDelay (ms) เท่าที่ทำได้ จากนั้นชดเชยด้วย delay us สั้น ๆ
    // แต่เพื่อไม่ให้ WDT โวย เราจะใช้ vTaskDelay อย่างเดียวเมื่อ >= 1ms
    if (wait_us >= 1000) {
        TickType_t dly = pdMS_TO_TICKS((uint32_t)(wait_us / 1000));
        if (dly > 0) vTaskDelay(dly);
    }
    // ชดเชยเศษที่เหลือด้วยการนอนอีกครั้งแบบสั้น ๆ
    // (เลี่ยง ets_delay_us ในลูป control ยาว ๆ)
    int64_t remain = *next_deadline_us - esp_timer_get_time();
    if (remain > 0 && remain < 1000) {
        // นอนอย่างน้อย 1 tick เพื่อยอมแพ้ CPU (ป้องกัน Idle ไม่ได้วิ่ง)
        // แล้วค่อยปล่อยให้รอบถัดไปแก้เฟสเอง (จะได้ไม่ busy)
        taskYIELD();
    }
}

/* ===================== Dummy workloads ===================== */
static float do_control_compute(uint32_t k)
{
    // งานคำนวณเล็ก ๆ ให้พอมีภาระ CPU
    volatile float acc = 0.f;
    for (int i = 0; i < 200; ++i) {  // อย่าเยอะเกินจนกินคาบเวลา
        acc += sqrtf((float)(i + 1)) * 0.001f;
    }
    return acc + (k & 0x7) * 0.01f;
}

static void do_daq_read(float *v1, float *v2)
{
    // จำลองอ่าน ADC/เซนเซอร์
    static float t = 0.f;
    t += 0.05f;
    *v1 = 1.23f + 0.1f * sinf(t);
    *v2 = 3.45f + 0.1f * cosf(t);
}

static void do_comm_io(void)
{
    // จำลอง I/O (เช่นส่ง MQTT/Socket) แบบไม่บล็อกยาว
    vTaskDelay(pdMS_TO_TICKS(5));
}

static void do_background_work(void)
{
    // งานเบา ๆ
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ====================== Tasks ======================= */

// Control 1 kHz @ Core0
static void control_task_core0(void *arg)
{
    ESP_LOGI(TAG, "Control task start on Core %d", xPortGetCoreID());

    period_stats_t stats;
    stats_init(&stats, CTRL_PERIOD_US);

    int64_t next_deadline_us = 0;
    int64_t last_report = esp_timer_get_time();
    uint32_t seq = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        // ทำคอนโทรล
        float u = do_control_compute(seq);

        // ส่งข้อความให้ Comm (บอกเวลาส่งเพื่อวัด latency)
        ctrl_msg_t m = {
            .t_send_us = t0,
            .seq = seq++,
            .ctrl_output = u
        };
        xQueueSend(q_ctrl_to_comm, &m, 0);

        // อัปเดตสถิติจังหวะ (จิตเตอร์)
        int64_t t1 = esp_timer_get_time();
        stats_update(&stats, t1);

        // รายงานทุก ๆ 1 วินาที
        if ((t1 - last_report) >= (REPORT_MS * 1000)) {
            stats_report_and_clear(TAG, "Control loop", &stats);
            // รีเซ็ตสถิติ
            stats_init(&stats, CTRL_PERIOD_US);
            last_report = t1;
        }

        // รักษาคาบเวลา
        delay_until_us(&next_deadline_us, CTRL_PERIOD_US);
    }
}

// Data Acquisition 500 Hz @ Core0
static void daq_task_core0(void *arg)
{
    ESP_LOGI(TAG, "DAQ task start on Core %d", xPortGetCoreID());

    period_stats_t stats;
    stats_init(&stats, DAQ_PERIOD_US);

    int64_t next_deadline_us = 0;
    int64_t last_report = esp_timer_get_time();

    while (1) {
        float a, b;
        do_daq_read(&a, &b);

        int64_t now = esp_timer_get_time();
        stats_update(&stats, now);

        if ((now - last_report) >= (REPORT_MS * 1000)) {
            stats_report_and_clear(TAG, "Data acquisition", &stats);
            stats_init(&stats, DAQ_PERIOD_US);
            last_report = now;
        }

        delay_until_us(&next_deadline_us, DAQ_PERIOD_US);
    }
}

// Communication @ Core1
static void comm_task_core1(void *arg)
{
    ESP_LOGI(TAG, "Comm task start on Core %d", xPortGetCoreID());

    uint32_t recv_count = 0;
    int64_t last_report = esp_timer_get_time();
    double lat_sum_ms = 0.0, lat_max_ms = 0.0;

    while (1) {
        ctrl_msg_t m;
        // รอข้อความจาก control (ให้เวลาบล็อกสั้น เพื่อยังทำงานอื่นได้)
        if (xQueueReceive(q_ctrl_to_comm, &m, pdMS_TO_TICKS(10)) == pdTRUE) {
            int64_t now = esp_timer_get_time();
            double lat_ms = (double)(now - m.t_send_us) / 1000.0;
            lat_sum_ms += lat_ms;
            if (lat_ms > lat_max_ms) lat_max_ms = lat_ms;
            recv_count++;
        }

        // ทำ I/O (จำลอง)
        do_comm_io();

        // รายงานทุก ๆ 1 วินาที
        int64_t now2 = esp_timer_get_time();
        if ((now2 - last_report) >= (REPORT_MS * 1000)) {
            if (recv_count > 0) {
                double avg_ms = lat_sum_ms / (double)recv_count;
                ESP_LOGI(TAG, "Communication latency: %.2f ms average (max: %.2f ms)",
                         avg_ms, lat_max_ms);
            } else {
                ESP_LOGI(TAG, "Communication latency: no messages");
            }
            recv_count = 0;
            lat_sum_ms = 0.0;
            lat_max_ms = 0.0;
            last_report = now2;
        }
    }
}

// Background (no affinity)
static void background_task(void *arg)
{
    ESP_LOGI(TAG, "Background task on Core %d", xPortGetCoreID());
    while (1) {
        do_background_work();
        // ตัวอย่าง log ห่าง ๆ
        static uint32_t n = 0;
        if ((++n % 20) == 0) {
            ESP_LOGI(TAG, "BG alive. Free heap ~ %d bytes", (int)esp_get_free_heap_size());
        }
    }
}

/* ===================== app_main ===================== */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Core-Pinned Real-Time Demo; Main on Core %d", xPortGetCoreID());

    // คิวสื่อสาร Control -> Comm
    q_ctrl_to_comm = xQueueCreate(32, sizeof(ctrl_msg_t));
    configASSERT(q_ctrl_to_comm != NULL);

    // สร้าง tasks ด้วย priority ภายใต้ 0..24
    BaseType_t ok;

    ok = xTaskCreatePinnedToCore(control_task_core0, "Ctrl_1kHz", STK_CTRL, NULL, PRIO_CTRL, NULL, CORE0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(daq_task_core0, "DAQ_500Hz", STK_DAQ, NULL, PRIO_DAQ, NULL, CORE0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(comm_task_core1, "Comm", STK_COMM, NULL, PRIO_COMM, NULL, CORE1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreate(background_task, "BG", STK_BG, NULL, PRIO_BG, NULL);
    configASSERT(ok == pdPASS);
}