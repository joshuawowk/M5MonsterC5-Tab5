/* Sub-GHz (CC1101) spectrum analyzer / waterfall screen. Sends subghz_spectrum
 * and paints [SUBGHZ_SPECTRUM] data=<hex> rows into a scrolling spectrogram with
 * a live peak frequency/RSSI readout. Reachable from the Radios submenu. */
#include "subghz_host.h"
#include "radio_waterfall.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "subghz_spectrum";
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
static uint8_t s_row[256];
static volatile int  s_row_n = 0;
static volatile bool s_row_dirty = false;
static volatile float s_peak_f = 0.0f;
static volatile int  s_peak_rssi = -200;
static volatile bool s_peak_dirty = false;

static int hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }

static void process_line(const char *line)
{
    const char *p = strstr(line, "[SUBGHZ_SPECTRUM] ");
    if (!p) return;
    float pk = 0; int pr = -200;
    const char *pp = strstr(p, "peak="); if (pp) sscanf(pp, "peak=%f", &pk);
    const char *rp = strstr(p, "prssi="); if (rp) sscanf(rp, "prssi=%d", &pr);
    const char *d = strstr(p, "data=");
    if (!d) return;
    d += 5;
    portENTER_CRITICAL(&s_lock);
    int n = 0;
    while (n < 256) {
        int hi = hexv(d[0]); if (hi < 0) break;
        int lo = hexv(d[1]); if (lo < 0) break;
        s_row[n++] = (uint8_t)((hi << 4) | lo); d += 2;
    }
    s_row_n = n; s_row_dirty = true;
    portEXIT_CRITICAL(&s_lock);
    s_peak_f = pk; s_peak_rssi = pr; s_peak_dirty = true;
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
    if (s_row_n > 0) {   /* scroll every tick (continuous), like the listen waterfall */
        uint8_t snap[256]; int n;
        portENTER_CRITICAL(&s_lock);
        n = s_row_n; memcpy(snap, s_row, n); s_row_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        radio_wf_push(&s_wf, snap, n);
    }
    if (s_peak_dirty && s_peak_lbl) {
        s_peak_dirty = false;
        lv_label_set_text_fmt(s_peak_lbl, "Peak: %.3f MHz  %d dBm", s_peak_f, s_peak_rssi);
    }
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("subghz_stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    radio_wf_free(&s_wf);
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_peak_lbl = NULL;
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }

/* ISM band presets: send subghz_stop then re-arm the spectrum sweep for one band. */
typedef struct { const char *label; const char *args; } spec_band_preset_t;
static const spec_band_preset_t k_spec_presets[4] = {
    { "315", "314.0 316.0 0.02" },
    { "433", "433.0 434.0 0.01" },
    { "868", "867.5 868.5 0.01" },
    { "915", "914.0 916.0 0.02" },
};

static void spectrum_send(const char *args)
{
    char cmd[64];
    subghz_host_uart_send("subghz_stop");
    snprintf(cmd, sizeof(cmd), "subghz_spectrum %s", args);
    subghz_host_uart_send(cmd);
}

static void on_band_preset(lv_event_t *e)
{
    const char *args = (const char *)lv_event_get_user_data(e);
    spectrum_send(args);
    if (s_peak_lbl) lv_label_set_text_fmt(s_peak_lbl, "Peak: --   (scanning %s MHz)", args);
}

void show_subghz_spectrum_page(void)
{
    lv_obj_t *container = subghz_host_current_container();
    if (!container) return;
    subghz_host_hide_all_pages();
    cleanup();   /* tear down any prior instance (timer/task/PSRAM) on re-entry */

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
    lv_label_set_text(title, "  Sub-GHz Spectrum");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_pink(), 0);

    s_peak_lbl = lv_label_create(s_page);
    lv_label_set_text(s_peak_lbl, "Peak: --   (scanning 433.0-434.0 MHz)");
    lv_obj_set_style_text_font(s_peak_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_peak_lbl, subghz_host_ui_muted(), 0);

    /* ISM band preset buttons: 315 / 433 / 868 / 915 MHz */
    lv_obj_t *preset_row = lv_obj_create(s_page);
    lv_obj_set_size(preset_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(preset_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(preset_row, 0, 0);
    lv_obj_set_style_pad_all(preset_row, 0, 0);
    lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preset_row, 8, 0);
    lv_obj_clear_flag(preset_row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pb = lv_btn_create(preset_row);
        lv_obj_set_size(pb, 84, 44);
        lv_obj_set_style_radius(pb, 8, 0);
        lv_obj_set_style_bg_color(pb, subghz_host_color_purple(), 0);
        lv_obj_add_event_cb(pb, on_band_preset, LV_EVENT_CLICKED, (void *)k_spec_presets[i].args);
        lv_obj_t *pl = lv_label_create(pb);
        lv_label_set_text(pl, k_spec_presets[i].label);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(pl);
    }

    if (!radio_wf_init(&s_wf, s_page, WF_W, WF_H)) ESP_LOGE(TAG, "waterfall alloc failed");

    s_tab_id = subghz_host_current_tab();
    s_alive = true;
    s_row_dirty = false; s_peak_dirty = false;
    xTaskCreate(reader_task, "sg_spec_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 100, NULL);

    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("subghz_spectrum 433.0 434.0 0.01");
    ESP_LOGI(TAG, "sub-GHz spectrum page ready");
}
