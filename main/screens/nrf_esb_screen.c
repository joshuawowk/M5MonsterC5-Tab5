/* nRF24 ESB / MouseJack sniffer screen. Sends init_nrf24 + nrf_esb_scan, lists
 * [NRF_ESB] devices (addr/ch/rate/dev) as they lock, and offers a Replay button
 * (nrf_esb_replay). Reachable from the Radios submenu. */
#include "subghz_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nrf_esb";
#define RXBUF 1024
#define LINEBUF 1024
#define MAX_ROWS 14

static lv_obj_t   *s_page = NULL;
static lv_obj_t   *s_list = NULL;
static lv_obj_t   *s_status = NULL;
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_alive = false;
static int s_tab_id = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_rows[MAX_ROWS][96];
static volatile int  s_nrows = 0;
static volatile bool s_dirty = false;
static volatile bool s_replayed = false;

static void add_row(const char *txt)
{
    portENTER_CRITICAL(&s_lock);
    if (s_nrows < MAX_ROWS) { snprintf(s_rows[s_nrows], sizeof(s_rows[s_nrows]), "%s", txt); s_nrows++; s_dirty = true; }
    portEXIT_CRITICAL(&s_lock);
}

static void process_line(const char *line)
{
    const char *p = strstr(line, "[NRF_ESB] ");
    if (p) {
        char addr[24] = "?"; int ch = 0, mhz = 0, len = 0; char rate[6] = "?", dev[16] = "?";
        sscanf(p, "[NRF_ESB] addr=%23s ch=%d mhz=%d rate=%5s len=%d dev=%15s",
               addr, &ch, &mhz, rate, &len, dev);
        char row[96];
        snprintf(row, sizeof(row), "%s  ch%d %s  %s (%dB)", addr, ch, rate, dev, len);
        add_row(row);
        return;
    }
    if (strstr(line, "[NRF_ESB_TX] ok=1")) s_replayed = true;
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
    if (s_dirty && s_list) {
        char snap[MAX_ROWS][96]; int n;
        portENTER_CRITICAL(&s_lock);
        n = s_nrows; memcpy(snap, s_rows, sizeof(char) * 96 * (n < MAX_ROWS ? n : MAX_ROWS)); s_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        lv_obj_clean(s_list);
        for (int i = 0; i < n; i++) {
            lv_obj_t *l = lv_label_create(s_list);
            lv_label_set_text(l, snap[i]);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(l, subghz_host_ui_text(), 0);
        }
        if (s_status) lv_label_set_text_fmt(s_status, "Sniffing... %d device(s)", n);
    }
    if (s_replayed && s_status) { s_replayed = false; lv_label_set_text(s_status, "Replayed last packet"); }
}

static void cleanup(void)
{
    if (s_alive) subghz_host_uart_send("stop");
    s_alive = false;
    for (int i = 0; i < 25 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    s_list = NULL; s_status = NULL;
    portENTER_CRITICAL(&s_lock); s_nrows = 0; portEXIT_CRITICAL(&s_lock);
}

static void on_back(lv_event_t *e){ (void)e; cleanup(); subghz_host_show_main_tiles(); }
static void on_replay(lv_event_t *e){ (void)e; subghz_host_uart_send("nrf_esb_replay"); }

void show_nrf_esb_page(void)
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
    lv_label_set_text(title, "  nRF24 ESB / MouseJack");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, subghz_host_color_cyan(), 0);

    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_flex_grow(spacer, 1); lv_obj_set_height(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(spacer, 0, 0);

    lv_obj_t *rbtn = lv_btn_create(header);
    lv_obj_set_style_bg_color(rbtn, subghz_host_color_amber(), 0);
    lv_obj_add_event_cb(rbtn, on_replay, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rbtn); lv_label_set_text(rl, "Replay last");

    s_status = lv_label_create(s_page);
    lv_label_set_text(s_status, "Sniffing...");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status, subghz_host_ui_muted(), 0);

    s_list = lv_obj_create(s_page);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_color(s_list, subghz_host_ui_panel(), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 10, 0);
    lv_obj_set_style_pad_all(s_list, 10, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    s_tab_id = subghz_host_current_tab();
    s_alive = true; s_dirty = false; s_replayed = false;
    portENTER_CRITICAL(&s_lock); s_nrows = 0; portEXIT_CRITICAL(&s_lock);
    xTaskCreate(reader_task, "nrf_esb_rd", 4096, NULL, 5, &s_task);
    s_timer = lv_timer_create(ui_tick, 150, NULL);

    subghz_host_uart_flush_input(s_tab_id);
    subghz_host_uart_send("init_nrf24");
    subghz_host_uart_send("nrf_esb_scan");
    ESP_LOGI(TAG, "nRF24 ESB page ready");
}
