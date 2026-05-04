#pragma once
#include <stdbool.h>

void touch_panel_init(void);
bool touch_panel_read(int *x, int *y);
void app_touch_start(void);
