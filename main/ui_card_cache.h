#pragma once

#include "card_profile.h"

void ui_card_cache_invalidate(void);

int ui_card_cache_total(bool only_unregistered);

/**
 * Nạp một trang danh sách thẻ (giống lcd_ui card_list_load).
 * Có thể chỉnh *page_in_out nếu vượt max trang (đồng bộ với g_card_page).
 */
int ui_card_cache_load_page(int *page_in_out, bool only_unregistered, CardProfileEntry_t *entries, int max_entries);
