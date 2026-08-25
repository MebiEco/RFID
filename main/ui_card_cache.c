#include "ui_card_cache.h"

#include <stddef.h>

extern int g_card_page;

void ui_card_cache_invalidate(void)
{
    /* card_profile đọc trực tiếp từ SD — không cache entry tại đây */
}

int ui_card_cache_total(bool only_unregistered)
{
    return card_profile_count_matched(only_unregistered, NULL);
}

int ui_card_cache_load_page(int *page_in_out, bool only_unregistered, CardProfileEntry_t *entries, int max_entries)
{
    if (!page_in_out || !entries || max_entries <= 0) {
        return 0;
    }
    int total = card_profile_count_matched(only_unregistered, NULL);
    int max_page = (total <= 0) ? 0 : (total - 1) / CARD_PROFILE_PAGE_ROWS;
    int page = *page_in_out;
    if (page > max_page) {
        page = max_page;
        g_card_page = max_page;
        *page_in_out = max_page;
    }
    int skip = page * CARD_PROFILE_PAGE_ROWS;
    return card_profile_list_page(entries, max_entries, only_unregistered, skip, NULL);
}
