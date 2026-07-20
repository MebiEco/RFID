#include "card_profile.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "board_pins.h"
#include "esp_log.h"
#include "scan_log.h"
#include "sd_card.h"
#include "esp_timer.h"

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

/** UID hex lien (bo :, -, space) — viet hoa nhu RFID. */
static void uid_normalize_inplace(char *uid, size_t uid_sz)
{
    if (!uid || uid_sz == 0) {
        return;
    }
    char tmp[24];
    size_t j = 0;
    for (size_t i = 0; uid[i] && j + 1 < sizeof(tmp); i++) {
        unsigned char c = (unsigned char)uid[i];
        if (c == ':' || c == '-' || isspace(c)) {
            continue;
        }
        if (isxdigit(c)) {
            tmp[j++] = (char)toupper(c);
        }
    }
    tmp[j] = '\0';
    snprintf(uid, uid_sz, "%s", tmp);
}

static bool remove_file_logged(const char *path, const char *what)
{
    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Da xoa %s: %s", what, path);
        return true;
    }
    if (errno != ENOENT) {
        ESP_LOGW(TAG, "remove %s %s: %s", what, path, strerror(errno));
    }
    return false;
}

/** Xoa <UID>.jpg trong profiles/ va goc the (ca viet hoa/thuong). */
static bool remove_profile_images(const char *uid)
{
    char path[CARD_PROFILE_PATH_MAX];
    char uid_lc[24];
    bool any = false;

    snprintf(path, sizeof(path), "%s/%s.jpg", BOARD_SD_PROFILES_DIR, uid);
    any |= remove_file_logged(path, "anh");

    strncpy(uid_lc, uid, sizeof(uid_lc) - 1);
    uid_lc[sizeof(uid_lc) - 1] = '\0';
    for (int i = 0; uid_lc[i]; i++) {
        uid_lc[i] = (char)tolower((unsigned char)uid_lc[i]);
    }
    if (strcmp(uid_lc, uid) != 0) {
        snprintf(path, sizeof(path), "%s/%s.jpg", BOARD_SD_PROFILES_DIR, uid_lc);
        any |= remove_file_logged(path, "anh");
    }

    snprintf(path, sizeof(path), "%s/%s.jpg", BOARD_SD_MOUNT_POINT, uid);
    any |= remove_file_logged(path, "anh (goc the)");

    snprintf(path, sizeof(path), "%s/%s.jpg", BOARD_SD_MOUNT_POINT, uid_lc);
    any |= remove_file_logged(path, "anh (goc the)");
    return any;
}

/** Bo UID khoi /sdcard/checkin/YYMMDD.txt — lan quet sau trong ngay lai la check-in (2.wav). */
static void checkin_remove_uid_today(const char *uid_nc)
{
    if (!uid_nc || !uid_nc[0] || !sd_card_is_mounted()) {
        return;
    }

    time_t now = time(NULL);
    struct tm ti;
    scan_log_wall_tm(now, &ti);
    if (ti.tm_year < (2020 - 1900)) {
        return;
    }

    char path[64];
    char tmp[72];
    snprintf(path, sizeof(path), "%s/%02d%02d%02d.txt", BOARD_SD_CHECKIN_DIR, (int)(ti.tm_year % 100),
             (int)(ti.tm_mon + 1), (int)ti.tm_mday);
    snprintf(tmp, sizeof(tmp), "%s/_chk.tmp", BOARD_SD_CHECKIN_DIR);

    sd_card_lock();
    FILE *fin = fopen(path, "r");
    if (!fin) {
        sd_card_unlock();
        return;
    }

    FILE *fout = fopen(tmp, "w");
    if (!fout) {
        fclose(fin);
        sd_card_unlock();
        ESP_LOGW(TAG, "checkin tmp: %s", strerror(errno));
        return;
    }

    char line[32];
    bool removed = false;
    while (fgets(line, sizeof(line), fin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strcmp(line, uid_nc) == 0) {
            removed = true;
            continue;
        }
        fprintf(fout, "%s\n", line);
    }
    fclose(fin);
    fclose(fout);

    if (!removed) {
        (void)remove(tmp);
        sd_card_unlock();
        return;
    }

    if (remove(path) != 0) {
        (void)remove(tmp);
        sd_card_unlock();
        ESP_LOGW(TAG, "checkin remove rename fail: %s", strerror(errno));
        return;
    }
    if (rename(tmp, path) != 0) {
        sd_card_unlock();
        ESP_LOGW(TAG, "checkin rename: %s", strerror(errno));
        return;
    }
    sd_card_unlock();
    ESP_LOGI(TAG, "Da xoa UID %s khoi checkin hom nay (%s)", uid_nc, path);
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

static bool wall_time_valid(time_t utc)
{
    struct tm t;
    scan_log_wall_tm(utc, &t);
    return t.tm_year >= (2020 - 1900);
}

static void card_profile_cleanup_unregistered(void)
{
    if (!sd_card_is_mounted()) {
        return;
    }

    sd_card_lock();
    DIR *dir = opendir(BOARD_SD_PROFILES_DIR);
    if (!dir) {
        sd_card_unlock();
        return;
    }

    char to_delete[100][24];
    int to_delete_cnt = 0;

    time_t now = time(NULL);
    long long current_uptime = esp_timer_get_time() / 1000000;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char uid[24];
        if (!uid_from_txt_name(ent->d_name, uid, sizeof(uid))) {
            continue;
        }

        char path[CARD_PROFILE_PATH_MAX];
        path_in_profiles(path, sizeof(path), uid);

        char name[48] = {0}, id[48] = {0};
        long long created_val = 0;
        long long uptime_val = 0;

        FILE *f = fopen(path, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                char buf_c[128];
                strncpy(buf_c, line, sizeof(buf_c) - 1);
                buf_c[sizeof(buf_c) - 1] = '\0';
                trim_inplace(buf_c);
                char *colon = strchr(buf_c, ':');
                if (colon) {
                    *colon = '\0';
                    char *key = buf_c;
                    trim_inplace(key);
                    char *val = colon + 1;
                    trim_inplace(val);
                    for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
                    if (strcmp(key, "name") == 0) {
                        snprintf(name, sizeof(name), "%s", val);
                        trim_inplace(name);
                    } else if (strcmp(key, "id") == 0) {
                        snprintf(id, sizeof(id), "%s", val);
                        trim_inplace(id);
                    } else if (strcmp(key, "created") == 0) {
                        created_val = strtoll(val, NULL, 10);
                    } else if (strcmp(key, "uptime") == 0) {
                        uptime_val = strtoll(val, NULL, 10);
                    }
                }
            }
            fclose(f);
        }

        bool reg = (name[0] != '\0' && name[0] != ' ' && id[0] != '\0' && id[0] != ' ');
        if (reg) {
            continue;
        }

        struct stat st;
        time_t mtime = 0;
        if (stat(path, &st) == 0) {
            mtime = st.st_mtime;
        }

        bool should_delete = false;

        if (uptime_val > 0 && current_uptime >= uptime_val) {
            if (current_uptime - uptime_val > 3600) {
                should_delete = true;
            }
        }

        if (!should_delete && created_val > 0 && wall_time_valid(now) && wall_time_valid(created_val)) {
            if (now - created_val > 3600) {
                should_delete = true;
            }
        }

        if (!should_delete && mtime > 0 && wall_time_valid(now) && wall_time_valid(mtime)) {
            if (now - mtime > 3600) {
                should_delete = true;
            }
        }

        if (should_delete) {
            if (to_delete_cnt < 100) {
                strncpy(to_delete[to_delete_cnt], uid, sizeof(to_delete[to_delete_cnt]) - 1);
                to_delete[to_delete_cnt][sizeof(to_delete[to_delete_cnt]) - 1] = '\0';
                to_delete_cnt++;
            }
        }
    }
    closedir(dir);
    sd_card_unlock();

    for (int i = 0; i < to_delete_cnt; i++) {
        ESP_LOGI("card_prof", "Tu dong xoa the la qua 1 tieng: %s", to_delete[i]);
        card_profile_delete(to_delete[i]);
    }
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
    fprintf(f, "Created:%lld\n", (long long)time(NULL));
    fprintf(f, "Uptime:%lld\n", (long long)(esp_timer_get_time() / 1000000));
    fclose(f);
    sd_card_unlock();
    return ESP_OK;
}

esp_err_t card_profile_lookup(const char *uid_hex_nocolon, char *name_out, size_t name_len, char *id_out,
                              size_t id_len, bool *registered_out, bool *created_file_out)
{
    card_profile_cleanup_unregistered();
    if (!uid_hex_nocolon || !uid_hex_nocolon[0] || !registered_out || !created_file_out) {
        return ESP_ERR_INVALID_ARG;
    }
    char uid[24];
    snprintf(uid, sizeof(uid), "%s", uid_hex_nocolon);
    uid_normalize_inplace(uid, sizeof(uid));
    if (uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *registered_out = false;
    *created_file_out = false;
    name_out[0] = '\0';
    id_out[0] = '\0';

    migrate_legacy_profiles_once();

    char path_prof[CARD_PROFILE_PATH_MAX];
    char path_old[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path_prof, sizeof(path_prof), uid);
    path_legacy_root(path_old, sizeof(path_old), uid);

    if (read_profile_file(path_prof, name_out, name_len, id_out, id_len) != ESP_OK &&
        read_profile_file(path_old, name_out, name_len, id_out, id_len) != ESP_OK) {
        if (write_template(path_prof, uid) == ESP_OK) {
            *created_file_out = true;
            ESP_LOGI(TAG, "Tao file: %s", path_prof);
        }
        *registered_out = false;
        return ESP_OK;
    }

    *registered_out = (name_out[0] != '\0' && name_out[0] != ' ' && id_out[0] != '\0' && id_out[0] != ' ');
    return ESP_OK;
}

esp_err_t card_profile_save(const char *uid_hex_nocolon, const char *name, const char *id)
{
    if (!uid_hex_nocolon || !uid_hex_nocolon[0]) return ESP_ERR_INVALID_ARG;
    char uid[24];
    snprintf(uid, sizeof(uid), "%s", uid_hex_nocolon);
    uid_normalize_inplace(uid, sizeof(uid));
    if (uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char path[CARD_PROFILE_PATH_MAX];
    char leg[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path, sizeof(path), uid);
    path_legacy_root(leg, sizeof(leg), uid);
    sd_card_lock();
    FILE *f = fopen(path, "w");
    if (!f) { sd_card_unlock(); return ESP_FAIL; }
    fprintf(f, "UID:%s\n", uid);
    fprintf(f, "Name : %s\n", name ? name : "");
    fprintf(f, "ID   : %s\n", id   ? id   : "");
    fflush(f);
    fclose(f);
    (void)remove(leg);
    sd_card_unlock();
    ESP_LOGI(TAG, "Luu profile %s: name=%s id=%s", uid, name, id);
    return ESP_OK;
}

esp_err_t card_profile_delete(const char *uid_hex_nocolon)
{
    if (!uid_hex_nocolon || !uid_hex_nocolon[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "xoa profile %s: SD chua gan", uid_hex_nocolon);
        return ESP_ERR_INVALID_STATE;
    }

    char uid[24];
    snprintf(uid, sizeof(uid), "%s", uid_hex_nocolon);
    uid_normalize_inplace(uid, sizeof(uid));
    if (uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char path[CARD_PROFILE_PATH_MAX];
    char leg[CARD_PROFILE_PATH_MAX];
    path_in_profiles(path, sizeof(path), uid);
    path_legacy_root(leg, sizeof(leg), uid);

    sd_card_lock();
    bool got_txt = remove_file_logged(path, "profile");
    got_txt |= remove_file_logged(leg, "profile (goc the)");
    bool got_img = remove_profile_images(uid);
    sd_card_unlock();

    checkin_remove_uid_today(uid);

    if (!got_txt && !got_img) {
        ESP_LOGW(TAG, "Khong tim thay profile/anh de xoa: %s", uid);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Xoa the %s: txt=%s anh=%s", uid, got_txt ? "co" : "khong", got_img ? "co" : "khong");
    return ESP_OK;
}

typedef struct {
    CardProfileEntry_t entry;
    time_t mtime;
} sorted_entry_t;

static int compare_entries_mtime_desc(const void *a, const void *b)
{
    time_t ta = ((const sorted_entry_t *)a)->mtime;
    time_t tb = ((const sorted_entry_t *)b)->mtime;
    if (ta < tb) {
        return 1;
    }
    if (ta > tb) {
        return -1;
    }
    return 0;
}

/** DS thẻ đã đăng ký: sắp mã NV A→Z (trống xuống cuối). */
static int compare_entries_id_asc(const void *a, const void *b)
{
    const sorted_entry_t *ra = (const sorted_entry_t *)a;
    const sorted_entry_t *rb = (const sorted_entry_t *)b;
    const bool ea = (ra->entry.id[0] == '\0');
    const bool eb = (rb->entry.id[0] == '\0');
    if (ea && !eb) {
        return 1;
    }
    if (!ea && eb) {
        return -1;
    }
    const int c = strcmp(ra->entry.id, rb->entry.id);
    if (c != 0) {
        return c;
    }
    return strcmp(ra->entry.uid, rb->entry.uid);
}

static bool id_matches_filter(const char *id, const char *filter)
{
    if (!filter || !filter[0]) {
        return true;
    }
    if (!id) {
        id = "";
    }
    const size_t nlen = strlen(filter);
    const size_t hlen = strlen(id);
    if (nlen == 0) {
        return true;
    }
    if (nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool ok = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = id[i + j];
            char b = filter[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

static int scan_profile_txt(int mode, int skip_first, CardProfileEntry_t *entries, int max_entries,
                            bool count_all, const char *id_filter)
{
    card_profile_cleanup_unregistered();
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

    int file_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char uid[20];
        if (uid_from_txt_name(ent->d_name, uid, sizeof(uid))) {
            file_count++;
        }
    }
    rewinddir(dir);

    if (file_count == 0) {
        closedir(dir);
        sd_card_unlock();
        return 0;
    }

    sorted_entry_t *temp = malloc(file_count * sizeof(sorted_entry_t));
    if (!temp) {
        ESP_LOGE(TAG, "Out of memory allocating %d sorted entries", file_count);
        closedir(dir);
        sd_card_unlock();
        return 0;
    }

    int matched_count = 0;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        char uid[20];
        if (!uid_from_txt_name(nm, uid, sizeof(uid))) {
            continue;
        }

        /* Feed watchdog */
        extern void lv_port_feed_wdt(void);
        lv_port_feed_wdt();

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

        bool reg = (name[0] != '\0' && name[0] != ' ' && id[0] != '\0' && id[0] != ' ');
        if (mode == 1 && reg) {
            continue;
        }
        if (mode == 2 && !reg) {
            continue;
        }
        if (!id_matches_filter(id, id_filter)) {
            continue;
        }

        struct stat st;
        time_t mtime = 0;
        char date_str[80] = "---";
        if (stat(path, &st) == 0) {
            mtime = st.st_mtime;
            if (mtime > 0) {
                struct tm ti;
                scan_log_wall_tm(mtime, &ti);
                if (ti.tm_year >= (2020 - 1900)) {
                    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d %02d:%02d:%02d",
                             (int)(ti.tm_year + 1900), (int)(ti.tm_mon + 1), (int)ti.tm_mday,
                             (int)ti.tm_hour, (int)ti.tm_min, (int)ti.tm_sec);
                }
            }
        }

        if (matched_count < file_count) {
            strncpy(temp[matched_count].entry.uid, uid, sizeof(temp[matched_count].entry.uid) - 1);
            temp[matched_count].entry.uid[sizeof(temp[matched_count].entry.uid) - 1] = '\0';
            strncpy(temp[matched_count].entry.name, name, sizeof(temp[matched_count].entry.name) - 1);
            temp[matched_count].entry.name[sizeof(temp[matched_count].entry.name) - 1] = '\0';
            strncpy(temp[matched_count].entry.id, id, sizeof(temp[matched_count].entry.id) - 1);
            temp[matched_count].entry.id[sizeof(temp[matched_count].entry.id) - 1] = '\0';
            strncpy(temp[matched_count].entry.date, date_str, sizeof(temp[matched_count].entry.date) - 1);
            temp[matched_count].entry.date[sizeof(temp[matched_count].entry.date) - 1] = '\0';
            temp[matched_count].entry.registered = reg;
            temp[matched_count].mtime = mtime;
            matched_count++;
        }
    }
    closedir(dir);

    if (matched_count > 1) {
        int (*cmp)(const void *, const void *) =
            (mode == 2) ? compare_entries_id_asc : compare_entries_mtime_desc;
        qsort(temp, matched_count, sizeof(sorted_entry_t), cmp);
    }

    int filled = 0;
    if (count_all) {
        filled = matched_count;
    } else {
        for (int i = skip_first; i < matched_count && filled < max_entries; i++) {
            entries[filled] = temp[i].entry;
            filled++;
        }
    }

    free(temp);
    sd_card_unlock();
    return filled;
}

int card_profile_count_matched(bool only_unregistered, const char *id_q)
{
    return scan_profile_txt(only_unregistered ? 1 : 2, 0, NULL, 0, true, id_q);
}

int card_profile_list_page(CardProfileEntry_t *entries, int max_entries, bool only_unregistered, int skip_first,
                           const char *id_q)
{
    return scan_profile_txt(only_unregistered ? 1 : 2, skip_first, entries, max_entries, false, id_q);
}

int card_profile_list(CardProfileEntry_t *entries, int max_entries, bool only_unregistered)
{
    return card_profile_list_page(entries, max_entries, only_unregistered, 0, NULL);
}

void card_profile_delete_all(void)
{
    if (!sd_card_is_mounted()) return;
    sd_card_lock();
    DIR *dir = opendir(BOARD_SD_PROFILES_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            char path[CARD_PROFILE_LONG_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", BOARD_SD_PROFILES_DIR, ent->d_name);
            remove(path);
        }
        closedir(dir);
    }
    sd_card_unlock();
    ESP_LOGI(TAG, "Da xoa tat ca the trong %s", BOARD_SD_PROFILES_DIR);
}
