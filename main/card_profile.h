#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * UID (hex khong dau ':') -> file BOARD_SD_PROFILES_DIR/<UID>.txt (vd /sdcard/profiles/A1B2....txt).
 * Lan dau mo SD: file .txt profile o goc the duoc chuyen vao thu muc profiles.
 * Neu chua co file: tao template trong profiles/, *registered_out=false
 * Neu co: parse Name/ID — registered khi ca hai sau trim deu khac rong.
 */
esp_err_t card_profile_lookup(const char *uid_hex_nocolon, char *name_out, size_t name_len, char *id_out,
                              size_t id_len, bool *registered_out, bool *created_file_out);

/** Ghi lai Name va ID vao file profile (giu nguyen dong UID:). */
esp_err_t card_profile_save(const char *uid_hex_nocolon, const char *name, const char *id);

/** Xoa file profile. */
esp_err_t card_profile_delete(const char *uid_hex_nocolon);

#define CARD_PROFILE_LIST_MAX 20
/** So dong hien thi tren mot trang man hinh danh sach the (LCD). */
#define CARD_PROFILE_PAGE_ROWS 5
typedef struct {
    char uid[20];
    char name[48];
    char id[48];
    bool registered;
} CardProfileEntry_t;

/**
 * Liet ke tat ca file .txt tren SD (toi da max_entries), bo qua skip_first ban ghi khop bo loc.
 * only_unregistered=true -> chi lay the chua co name/id (the la).
 */
int card_profile_list_page(CardProfileEntry_t *entries, int max_entries, bool only_unregistered, int skip_first);

/** Dem so file .txt khop bo loc (dung cho phan trang). */
int card_profile_count_matched(bool only_unregistered);

/**
 * Liet ke tat ca file .txt tren SD (toi da CARD_PROFILE_LIST_MAX), tuong duong list_page(..., 0).
 * only_unregistered=true -> chi lay the chua co name/id (the la).
 * Tra ve so luong phan tu da dien vao entries[].
 */
int card_profile_list(CardProfileEntry_t *entries, int max_entries, bool only_unregistered);
