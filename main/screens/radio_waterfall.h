/* Reusable spectrogram/waterfall widget for the ported radio screens.
 * Each pushed row scrolls the image down one line and paints a new top row,
 * mapping n intensity bytes (0..255) across the canvas width with a
 * blue->cyan->green->yellow->red colormap. RGB565 buffer in PSRAM. */
#pragma once
#include "lvgl.h"

typedef struct { lv_obj_t *canvas; uint16_t *buf; int w, h; } radio_wf_t;

/* Create a w x h RGB565 canvas as a child of parent. Returns false on OOM. */
bool radio_wf_init(radio_wf_t *wf, lv_obj_t *parent, int w, int h);
/* Scroll down one row and paint the newest row from n intensities (0..255). */
void radio_wf_push(radio_wf_t *wf, const uint8_t *intens, int n);
/* Delete the canvas + free the buffer. */
void radio_wf_free(radio_wf_t *wf);
