#include "card_profile.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "esp_log.h"
#include "sd_card.h"

static const char *TAG = "card_prof";

/** Du mount point + ten file (readdir toi ~NAME_MAX); tranh -Wformat-truncation tren path[80]. */
#define CARD_PROFILE_PATH_MAX 256
/** Goc the + ten LFN day du — dung khi migrate/rename, > CARD_PROFILE_PATH_MAX. */
#define CARD_PROFILE_LONG_PATH_MAX 512

static void path_in_profiles(char *out, size_t outsz, const char *uid)
{
    snprintf(out, outsz, "%s/%s.txt", BOARD_SD_PROFILES_DIR, uid);
}

static void path_legacy_root(char *out, size_t outsz, const char *uid)
{
    snprintf(out, outsz, "%s/%s.txt", BOARD_SD_MOUNT_POINT, uid);
}

static bool uid_from_txt_name(const char *nm, char *uid_out, size_t uid_out_sz)
{
    size_t len = strlen(nm);
    if (len < 5) {
        return false;
    }
    if (strcasecmp(nm + len - 4, ".txt") != 0) {
        return false;
    }
    size_t uid_len = len - 4;
    if (uid_len == 0 || uid_len >= uid_out_sz) {
        return false;
    }
    for (size_t i = 0; i < uid_len; i++) {
        if (!isxdigit((unsigned char)nm[i])) {
            return false;
        }
    }
    memcpy(uid_out, nm, uid_len);
    uid_out[uid_len] = '\0';
    return true;
}

/** Mot lan: chuyen <UID>.txt o goc the vao /profiles/ (giong cach dung thu muc /audio/). */
static void migrate_legacy_profiles_once(void)
{
    static bool done;
    if (done || !sd_card_is_mounted()) {
        return;
    }

    sd_card_lock();
    DIR *dir = opendir(BOARD_SD_MOUNT_POINT);
    if (!dir) {
        sd_card_unlock();
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        char uid[20];
        if (!uid_from_txt_name(nm, uid, sizeof(uid))) {
            continue;
        }

        char newpath[CARD_PROFILE_LONG_PATH_MAX];
        char oldpath[CARD_PROFILE_LONG_PATH_MAX];
        path_in_profiles(newpath, sizeof(newpath), uid);
        (void)snprintf(oldpath, sizeof(oldpath), "%s/%s", BOARD_SD_MOUNT_POINT, nm);

        FILE *already = fopen(newpath, "r");
        if (already) {
            fclose(already);
            (void)remove(oldpath);
            continue;
        }
        if (rename(oldpath, newpath) == 0) {
            ESP_LOGI(TAG, "Chuyen vao profiles/: %s", nm);
        }
    }
    closedir(dir);
    done = true;
    sd_card_unlock();
}

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

/** Mot dong dang "Name : x" hoac "ID   : y" — lay key truoc ':' (trim), value sau. */
static void parse_line_keys(const char *line, char *name_out, size_t name_len, char *id_out, size_t id_len)
{
    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim_inplace(buf);
    if (buf[0] == '\0' || buf[0] == '#') {
        return;
    }

    char *colon = strchr(buf, ':');
    if (!colon) {
        return;
    }
    *colon = '\0';
    char *key = buf;
    trim_inplace(key);
    const char *val = colon + 1;
    while (*val && isspace((unsigned char)*val)) {
        val++;
    }
    char valcpy[96];
    snprintf(valcpy, sizeof(valcpy), "%s", val);
    trim_inplace(valcpy);

    for (char *k = key; *k; k++) {
        *k = (char)tolower((unsigned char)*k);
    }

    if (strcmp(key, "name") == 0) {
        snprintf(name_out, name_len, "%s", valcpy);
    } else if (strcmp(key, "id") == 0) {
        snprintf(id_out, id_len, "%s", valcpy);
    }
}

static esp_err_t read_profile_file(const char *path, char *name_out, size_t name_len, char *id_out, size_t id_len)
{
    sd_card_lock();
    FILE *f = fopen(path, "r");
    if (!f) {
        sd_card_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    name_out[0] = '\0';
    id_out[0] = '\0';

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        parse_line_keys(line, name_out, name_len, id_out, id_len);
    }
    fclose(f);
    sd_card_unlock();
    trim_inplace(name_out);
    trim_inplace(id_out);
    return ESP_OK;
}

static esp_err_t write_template(const char *path, const char *uid_hex)
{
    sd_card_lock();
    FILE *f = fopen(path, "w");
    if (!f) {
        sd_card_unlock();
        ESP_LOGE(TAG, "fopen w %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "UID:%s\n", uid_hex);
    fprintf(f, "Name : \n");
    fprintf(f, "ID   : \n");
    fclose(f);
    sd_card_unlock();
    return ESP_OK;
}

esp_err_t card_profile_lookup(const char *uid_hex_nocolon, char *name_out, size_t name_len, char *id_out,
                              size_t id_len, bool *registered_out, bool *created_file_out)
{
    if (!uid_hex_nocolon || !uid_hex_nocolon[0] || !registered_out || !created_file_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *registered_out = false;
    *created_file_out = false;
    name_out[0] = '\0';
    id_out[0] = '\0';

    migrate_legacy_profiles_once();

    char path_prof[CARD_PROFILE_PATH_MAX];
    char path_old[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path_prof, sizeof(path_prof), uid_hex_nocolon);
    path_legacy_root(path_old, sizeof(path_old), uid_hex_nocolon);

    if (read_profile_file(path_prof, name_out, name_len, id_out, id_len) != ESP_OK &&
        read_profile_file(path_old, name_out, name_len, id_out, id_len) != ESP_OK) {
        if (write_template(path_prof, uid_hex_nocolon) == ESP_OK) {
            *created_file_out = true;
            ESP_LOGI(TAG, "Tao file: %s", path_prof);
        }
        *registered_out = false;
        return ESP_OK;
    }

    *registered_out = (name_out[0] != '\0' && id_out[0] != '\0');
    return ESP_OK;
}

esp_err_t card_profile_save(const char *uid_hex_nocolon, const char *name, const char *id)
{
    if (!uid_hex_nocolon || !uid_hex_nocolon[0]) return ESP_ERR_INVALID_ARG;
    char path[CARD_PROFILE_PATH_MAX];
    char leg[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path, sizeof(path), uid_hex_nocolon);
    path_legacy_root(leg, sizeof(leg), uid_hex_nocolon);
    sd_card_lock();
    FILE *f = fopen(path, "w");
    if (!f) { sd_card_unlock(); return ESP_FAIL; }
    fprintf(f, "UID:%s\n", uid_hex_nocolon);
    fprintf(f, "Name : %s\n", name ? name : "");
    fprintf(f, "ID   : %s\n", id   ? id   : "");
    fflush(f);
    fclose(f);
    (void)remove(leg);
    sd_card_unlock();
    ESP_LOGI(TAG, "Luu profile %s: name=%s id=%s", uid_hex_nocolon, name, id);
    return ESP_OK;
}

esp_err_t card_profile_delete(const char *uid_hex_nocolon)
{
    if (!uid_hex_nocolon || !uid_hex_nocolon[0]) return ESP_ERR_INVALID_ARG;
    char path[CARD_PROFILE_PATH_MAX];
    char leg[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path, sizeof(path), uid_hex_nocolon);
    path_legacy_root(leg, sizeof(leg), uid_hex_nocolon);
    sd_card_lock();
    int r1 = remove(path);
    int r2 = remove(leg);
    sd_card_unlock();
    if (r1 != 0 && r2 != 0) {
        ESP_LOGW(TAG, "remove profile %s failed", uid_hex_nocolon);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Da xoa profile %s", uid_hex_nocolon);
    return ESP_OK;
}

/**
 * skip_first: bo qua ban ghi khop bo loc dau tien.
 * entries NULL + max_entries==0 + count_all true -> dem tat ca khop (khong bo qua skip).
 */
static int scan_profile_txt(bool only_unregistered, int skip_first, CardProfileEntry_t *entries, int max_entries,
                            bool count_all)
{
    migrate_legacy_profiles_once();

    if (!sd_card_is_mounted()) {
        return 0;
    }
    if (!count_all && (!entries || max_entries <= 0)) {
        return 0;
    }

    sd_card_lock();
    DIR *dir = opendir(BOARD_SD_PROFILES_DIR);
    if (!dir) {
        sd_card_unlock();
        return 0;
    }

    int skip = skip_first;
    int filled = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        char uid[20];
        if (!uid_from_txt_name(nm, uid, sizeof(uid))) {
            continue;
        }

        char path[CARD_PROFILE_PATH_MAX];
        path_in_profiles(path, sizeof(path), uid);

        char name[48] = {0}, id[48] = {0};
        FILE *f = fopen(path, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                parse_line_keys(line, name, sizeof(name), id, sizeof(id));
            }
            fclose(f);
        }
        trim_inplace(name);
        trim_inplace(id);

        bool reg = (name[0] != '\0' && id[0] != '\0');
        if (only_unregistered && reg) {
            continue;
        }

        if (skip > 0) {
            skip--;
            continue;
        }

        if (count_all) {
            filled++;
            continue;
        }

        if (filled >= max_entries) {
            break;
        }

        strncpy(entries[filled].uid, uid, sizeof(entries[filled].uid) - 1);
        strncpy(entries[filled].name, name, sizeof(entries[filled].name) - 1);
        strncpy(entries[filled].id, id, sizeof(entries[filled].id) - 1);
        entries[filled].registered = reg;
        filled++;
    }
    closedir(dir);
    sd_card_unlock();
    return filled;
}

int card_profile_count_matched(bool only_unregistered)
{
    return scan_profile_txt(only_unregistered, 0, NULL, 0, true);
}

int card_profile_list_page(CardProfileEntry_t *entries, int max_entries, bool only_unregistered, int skip_first)
{
    return scan_profile_txt(only_unregistered, skip_first, entries, max_entries, false);
}

int card_profile_list(CardProfileEntry_t *entries, int max_entries, bool only_unregistered)
{
    return card_profile_list_page(entries, max_entries, only_unregistered, 0);
}
