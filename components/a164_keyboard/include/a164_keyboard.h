/*
 * M5Stack Tab5 A164 keyboard (SKU A164) driver for ESP-IDF.
 *
 * The A164 is a 70-key keyboard expansion for the Tab5 with an STM32F030 MCU
 * that exposes an I2C slave (addr 0x6D) on the Tab5's internal ExtPort1/J9:
 *   SDA = GPIO0 (J9-7)   SCL = GPIO1 (J9-8)   INT = GPIO50 (J9-10)
 * This driver puts it in HID mode and feeds keystrokes to LVGL as a KEYPAD
 * input device, so physical typing lands in whatever text field is bound via
 * a164_kbd_route_to(). It is completely independent of the USB-A host port used
 * for the MonsterC5 CDC link, so both can be used at the same time.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the A164 keyboard: create its private I2C bus, an LVGL KEYPAD
 *        indev + dedicated group, and a polling task. Safe to call once, after
 *        the display/LVGL are started. If the keyboard is not attached yet, the
 *        task keeps re-probing so it works when docked later (hot-plug).
 *
 * @param disp  the LVGL display the keypad indev is associated with.
 * @return ESP_OK on success (even if the keyboard is not currently attached).
 */
esp_err_t a164_keyboard_init(lv_display_t *disp);

/**
 * @brief Route physical keystrokes to a text area (the currently-focused field).
 *        Pass NULL to clear. Called from the on-screen-keyboard bind wrapper so
 *        the physical keyboard follows the same field as the touch keyboard.
 */
void a164_kbd_route_to(lv_obj_t *ta);

/** @return true if the A164 answered on I2C at least once. */
bool a164_kbd_present(void);

/**
 * @brief Switch arrow-key behavior. In nav mode the arrow keys move menu focus
 *        (NEXT/PREV); otherwise they act as text-cursor keys. The app's nav
 *        timer sets this based on whether a text field is active.
 */
void a164_kbd_set_nav_mode(bool on);

/** @return the LVGL group the keypad indev feeds (for the app to populate with
 *  the current screen's focusable widgets during menu navigation). */
lv_group_t *a164_kbd_group(void);

#ifdef __cplusplus
}
#endif
