#include "radio_waterfall.h"
#include "esp_heap_caps.h"
#include <string.h>

static inline uint16_t rwf_u16(uint32_t hex) { return lv_color_to_u16(lv_color_hex(hex)); }

/* intensity 0..255 -> RGB565 across a 6-stop gradient (dark blue -> red). */
static uint16_t rwf_color(uint8_t v)
{
    static const uint8_t stops[6][3] = {
        { 10, 22, 40 }, { 0, 80,160 }, { 0,200,200 },
        {120,220, 60 }, {255,220, 40 }, {255, 80, 40 },
    };
    float t = (float)v / 255.0f;
    float seg = t * 5.0f;
    int i = (int)seg; if (i > 4) i = 4;
    float f = seg - (float)i;
    int r = (int)(stops[i][0] + (stops[i+1][0] - stops[i][0]) * f + 0.5f);
    int g = (int)(stops[i][1] + (stops[i+1][1] - stops[i][1]) * f + 0.5f);
    int b = (int)(stops[i][2] + (stops[i+1][2] - stops[i][2]) * f + 0.5f);
    return rwf_u16(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

bool radio_wf_init(radio_wf_t *wf, lv_obj_t *parent, int w, int h)
{
    wf->canvas = NULL; wf->buf = NULL; wf->w = w; wf->h = h;
    size_t sz = (size_t)w * h * sizeof(uint16_t);
    wf->buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wf->buf) wf->buf = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (!wf->buf) return false;
    uint16_t bg = rwf_color(0);
    for (int i = 0; i < w * h; i++) wf->buf[i] = bg;
    wf->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(wf->canvas, wf->buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(wf->canvas, w, h);
    return true;
}

void radio_wf_push(radio_wf_t *wf, const uint8_t *intens, int n)
{
    if (!wf->buf || n <= 0) return;
    int w = wf->w, h = wf->h;
    /* scroll every row down by one */
    for (int y = h - 1; y >= 1; y--)
        memcpy(&wf->buf[y * w], &wf->buf[(y - 1) * w], (size_t)w * sizeof(uint16_t));
    /* paint newest row (row 0), mapping n bins across the width */
    for (int x = 0; x < w; x++) {
        int bin = (int)((long)x * n / w);
        if (bin >= n) bin = n - 1;
        wf->buf[x] = rwf_color(intens[bin]);
    }
    if (wf->canvas) lv_obj_invalidate(wf->canvas);
}

void radio_wf_free(radio_wf_t *wf)
{
    if (wf->canvas) { lv_obj_delete(wf->canvas); wf->canvas = NULL; }
    if (wf->buf) { heap_caps_free(wf->buf); wf->buf = NULL; }
}
