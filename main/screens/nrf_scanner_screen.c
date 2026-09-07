/* nRF24 2.4 GHz scanner / spectrum analyzer screen. Sends init_nrf24 + nrf_scan
 * and paints [NRF_SPECTRUM] data=<hex> rows into a scrolling spectrogram, with a
 * live peak-channel readout. Reachable from the Radios submenu. */
#include "subghz_host.h"
#include "radio_waterfall.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nrf_scanner";
#define RXBUF 1024
#define LINEBUF 1024
#define WF_W 680
#define WF_H 250

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_peak_lbl = NULL;
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static radio_wf_t s_wf;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_row[128];
static volatile int  s_row_n = 0;
static volatile bool s_row_dirty = false;
static volatile int  s_peak_ch = -1, s_peak_mhz = 0, s_peak_pct = 0;
static volatile bool s_peak_dirty = false;

static int hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }

static void process_line(const char *line)
{
    const char *p;
    if ((p = strstr(line, "[NRF_SCAN_TOP] "))) {
        int ch=-1, mhz=0, pct=0;
        sscanf(p, "[NRF_SCAN_TOP] ch=%d mhz=%d pct=%d", &ch, &mhz, &pct);
        s_peak_ch = ch; s_peak_mhz = mhz; s_peak_pct = pct; s_peak_dirty = true;
        return;
    }
    if ((p = strstr(line, "[NRF_SPECTRUM] "))) {
        const char *d = strstr(p, "data=");
        if (!d) return;
        d += 5;
        portENTER_CRITICAL(&s_lock);
        int n = 0;
        while (n < 128) {
            int hi = hexv(d[0]); if (hi < 0) break;
            int lo = hexv(d[1]); if (lo < 0) break;
            s_row[n++] = (uint8_t)((hi << 4) | lo); d += 2;
        }
        s_row_n = n; s_row_dirty = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void reader_task(void *arg)
{
    (void)arg;
    static char rx[RXBUF], line[LINEBUF];
    int lp = 0;
    while (s_alive) {
        int len = subghz_host_uart_read_bytes(s_tab_id, rx, sizeof(rx) - 1, pdMS_TO_TICKS(100));
        if (len <= 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        rx[len] = '\0';
        for (int i = 0; i < len; i++) {
            char c = rx[i];
            if (c == '\n' || c == '\r') {
                if (lp > 0) { line[lp] = '\0'; process_line(line); lp = 0; }
            } else if (lp < LINEBUF - 1) line[lp++] = c;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    if (s_row_dirty) {
        uint8_t snap[128]; int n;
        portENTER_CRITICAL(&s_lock);
        n = s_row_n; memcpy(snap, s_row, n); s_row_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        radio_wf_push(&s_wf, snap, n);
    }
    if (s_peak_dirty && s_peak_lbl) {
        s_peak_dirty = false;
        if (s_peak_ch >= 0)
            lv_label_set_text_fmt(s_peak_lbl, "Peak: ch %d  (%d MHz)  %d%%", s_peak_ch, s_peak_mhz, s_peak_pct);
    }
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    radio_wf_free(&s_wf);
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_peak_lbl = NULL;
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

void show_nrf_scanner_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }

    s_page = lv_obj_create(container);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, subghz_host_ui_bg(), 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 10, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_page, 8, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(s_page);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(header);
    lv_obj_set_style_bg_color(back, subghz_host_ui_card(), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "  nRF24 2.4 GHz Scanner");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_cyan(), 0);

    s_peak_lbl = lv_label_create(s_page);
    lv_label_set_text(s_peak_lbl, "Peak: --");
    lv_obj_set_style_text_font(s_peak_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_peak_lbl, subghz_host_ui_muted(), 0);

    if (!radio_wf_init(&s_wf, s_page, WF_W, WF_H)) {
        ESP_LOGE(TAG, "waterfall alloc failed");
    }

    s_tab_id = subghz_host_current_tab();
    s_alive = true;
    s_row_dirty = false; s_peak_dirty = false; s_peak_ch = -1;
    xTaskCreate(reader_task, "nrf_scan_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 80, NULL);

    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("init_nrf24");
    subghz_host_uart_send("nrf_scan 0 125");
    ESP_LOGI(TAG, "nRF24 scanner page ready");
}
