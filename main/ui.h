/*
 * ui.h - 480x480 touchscreen UI (LVGL 9).
 *
 *  +-------------------------------------------+
 *  | Sun Oct 25  9:41 PM          [Practicing] |
 *  |                  12:34                     |
 *  |        Cello - detected (94%)      [End]    |
 *  |  [      Cello      ][      Piano      ]   |
 *  |  Today                         42 min     |
 *  |  [=========goal bar======          ]      |
 *  |  Cello 30 min  ·  Piano 12 min            |
 *  |  ▂ ▅ ▃ ▇ ▁ ▆ █   (last 7 days)            |
 *  |  Mic [====   ]   Sensitivity [---o--]  IP |
 *  +-------------------------------------------+
 */
#pragma once

#include "app.h"

/* Call once, with the display lock held.
 * The Cello/Piano row shows the detected instrument; tapping it overrides the
 * running session, or corrects the session that just ended. */
void ui_init(float sens_db);
