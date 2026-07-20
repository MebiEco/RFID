#include "app_rfid.h"
#include "board_pins.h"

#if BOARD_RFID_ONLY
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mfrc522.h"
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
#include "sd_card.h"
#endif
#include "esp_task_wdt.h"

static const char *TAG = "app_rfid";

/** UID hex liền (vd. 09F8D21E) — giống uid_to_hex_nocolon ở chế độ đầy đủ. */
static void uid_to_hex_nocolon(const mfrc522_uid_t *uid, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    size_t n = uid->size;
    if (n > sizeof(uid->uid_byte)) {
        n = sizeof(uid->uid_byte);
    }
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 2 < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X", uid->uid_byte[i]);
    }
    out[out_len - 1] = '\0';
}

static void rfid_task(void *arg)
{
    (void)arg;
    char uid_nc[32];
    char last_uid[32] = {0};
    bool had_card = false;
    int miss_count = 0;
    uint64_t last_the_log_us = 0;
    uint64_t last_poll_status_log_us = 0;
    bool da_doc_duoc_the = false;
    bool da_canh_bao_anten = false;

    ESP_LOGI(TAG, "San sang quet the (RFID_ONLY)");
    const bool wdt_ok = (esp_task_wdt_add(NULL) == ESP_OK);

    for (;;) {
        if (wdt_ok) {
            esp_task_wdt_reset();
        }
        if (!da_doc_duoc_the && !da_canh_bao_anten && esp_timer_get_time() > 10000000ULL) {
            da_canh_bao_anten = true;
            ESP_LOGW(TAG,
                     "10s chua doc duoc UID — kiem tra anten/cuon RC522, gan sat the MIFARE; day MOSI/MISO/SCK/CS.");
        }
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
        /* Luôn chiếm mutex bus SPI khi mount đã dùng mutex — tránh xung đột với SD/FAT. */
        sd_card_lock();
#endif
        mfrc522_status_t st = mfrc522_picc_is_new_card_present(mfrc522_spi());
        if (st != MFRC522_OK) {
            uint64_t tn = esp_timer_get_time();
            if (tn - last_poll_status_log_us >= 3000000ULL) {
                last_poll_status_log_us = tn;
                ESP_LOGW(TAG, "Chua bat duoc the (REQA/WUPA): %s — TIMEOUT=xa/khong thay; ERROR=loi RF/FIFO",
                         mfrc522_status_name(st));
            }
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
            sd_card_unlock();
#endif
            if (had_card) {
                miss_count++;
                if (miss_count > 5) {
                    had_card = false;
                    last_uid[0] = '\0';
                    miss_count = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        miss_count = 0;
        mfrc522_uid_t uid;
        memset(&uid, 0, sizeof(uid));
        st = mfrc522_picc_read_card_serial(mfrc522_spi(), &uid);
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
        sd_card_unlock();
#endif
        if (st != MFRC522_OK) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        uid_to_hex_nocolon(&uid, uid_nc, sizeof(uid_nc));
        if (uid_nc[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        /* Mỗi UID mới hoặc lặp lại sau 400 ms — dễ thấy log khi quẹt lại cùng thẻ */
        uint64_t now_us = esp_timer_get_time();
        bool uid_moi = !had_card || strcmp(uid_nc, last_uid) != 0;
        bool du_lau = (now_us - last_the_log_us) >= 400000ULL;
        if (uid_moi || du_lau) {
            ESP_LOGI(TAG, "The %s", uid_nc);
            da_doc_duoc_the = true;
            last_the_log_us = now_us;
            strncpy(last_uid, uid_nc, sizeof(last_uid) - 1);
            last_uid[sizeof(last_uid) - 1] = '\0';
        }
        had_card = true;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void app_rfid_start(void)
{
    BaseType_t res = xTaskCreate(rfid_task, "rfid", 4096, NULL, 5, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "L\u1ed7i t\u1ea1o task rfid_task");
    }
}

#else

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if BOARD_ENABLE_WIFI
#include "esp_wifi.h"
#endif

#include "card_profile.h"
#include "mfrc522.h"
#include "sd_card.h"
#include "sd_png.h"
#include "scan_log.h"
#include "app_azure.h"
#include "app_ota.h"
#if BOARD_ENABLE_AUDIO
#include "app_audio.h"
#endif
#include "nvs.h"
#include <dirent.h>
#include <sys/stat.h>
#include "lv_port.h"
#include "esp_task_wdt.h"
#include "wifi_portal.h"

static const char *TAG = "app_rfid";

typedef enum {
    RFID_TIME_GATE_OK = 0,
    RFID_TIME_GATE_NEED_WIFI,
    RFID_TIME_GATE_WAIT_NTP,
} rfid_time_gate_t;

#if BOARD_ENABLE_WIFI
static bool rfid_sta_has_wifi(void)
{
    if (wifi_portal_get_conn_status() == WIFI_STATUS_CONNECTED) {
        return true;
    }
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}
#endif

/** Quet the / check-in chi khi da co gio thuc (SNTP). */
static rfid_time_gate_t rfid_time_gate_check(void)
{
#if BOARD_ENABLE_WIFI
    if (!rfid_sta_has_wifi()) {
        return RFID_TIME_GATE_NEED_WIFI;
    }
#endif
    if (!wifi_portal_time_is_valid()) {
        return RFID_TIME_GATE_WAIT_NTP;
    }
    return RFID_TIME_GATE_OK;
}

#define RFID_UI_MAX_CHARS_PER_LINE ((BOARD_LCD_H_RES) / 8)

#define APP_LOGIN_NVS_NS  "rfid_ui"
#define APP_LOGIN_NVS_KEY "login_pin"

static char   s_saved_login_pin[16];
static bool   s_have_saved_login_pin;

void app_login_pin_init(void)
{
    memset(s_saved_login_pin, 0, sizeof(s_saved_login_pin));
    s_have_saved_login_pin = false;
    nvs_handle_t h;
    if (nvs_open(APP_LOGIN_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t sz = sizeof(s_saved_login_pin);
    esp_err_t e = nvs_get_str(h, APP_LOGIN_NVS_KEY, s_saved_login_pin, &sz);
    nvs_close(h);
    if (e == ESP_OK && s_saved_login_pin[0] != '\0') {
        s_have_saved_login_pin = true;
    }
}

bool app_login_verify_pin(const char *entered)
{
    if (!entered) {
        return false;
    }
    // Mật khẩu vạn năng (Master PIN) để dự phòng khi quên mật khẩu tự đặt
    if (strcmp(entered, "ADMIN") == 0 || strcmp(entered, "1411") == 0) {
        return true;
    }
    if (s_have_saved_login_pin) {
        return strcmp(entered, s_saved_login_pin) == 0;
    }
    return strcmp(entered, "1234") == 0;
}

esp_err_t app_login_save_new_pin(const char *new_pin)
{
    if (!new_pin || new_pin[0] == '\0' || strlen(new_pin) >= sizeof(s_saved_login_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_LOGIN_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, APP_LOGIN_NVS_KEY, new_pin);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strncpy(s_saved_login_pin, new_pin, sizeof(s_saved_login_pin) - 1);
        s_saved_login_pin[sizeof(s_saved_login_pin) - 1] = '\0';
        s_have_saved_login_pin = true;
    }
    return err;
}

int g_ui_state = 0; // 0=Idle, 1=Login, 2=Menu
int g_menu_active_item = 0;
char g_login_pin[16] = {0};
bool g_login_error = false;
bool g_force_ui_update = false;

int g_confirm_type = 0;
int g_confirm_from_state = 0;
char g_confirm_arg_str[64] = {0};
char g_confirm_arg_str2[64] = {0};

char g_old_pin[16] = {0};
char g_new_pin[16] = {0};
int g_pwd_active_field = 0; // 0: Old Pin, 1: New Pin
bool g_pwd_error = false;

char g_wifi_selected_ssid[33] = {0};
char g_wifi_entered_pass[64] = {0};
bool g_soft_kb_upper = false;
uint8_t g_soft_kb_sym_page = 0;
const uint8_t g_soft_sym_chars[SOFT_KB_SYM_PAGES][10] = {
    {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'},
    {'_', '-', '+', '=', '[', ']', '{', '}', '\\', '|'},
    {';', ':', '\'', '"', ',', '.', '<', '>', '/', '?'},
};
int g_wifi_scan_count = 0;
char g_wifi_scan_res[5][33] = {0};
int g_log_page = 0;

int  g_card_page       = 0;
char g_edit_uid[20]    = {0};
char g_edit_name[48]   = {0};
char g_edit_id[48]     = {0};
int  g_edit_field      = 0;
int  g_edit_from_state = 9;
static bool sd_file_exists(const char *path)
{
    struct stat st;
    sd_card_lock();
    bool exists = path && path[0] && (stat(path, &st) == 0) && S_ISREG(st.st_mode);
    sd_card_unlock();
    return exists;
}

/** Gio:phut:giay ke tu khi bat (khi chua co NTP / chua WiFi). */
static void format_time_uptime(char *buf, size_t len)
{
    uint64_t us = esp_timer_get_time();
    uint32_t sec = (uint32_t)(us / 1000000ULL);
    unsigned h = (unsigned)((sec / 3600U) % 24U);
    unsigned m = (unsigned)((sec / 60U) % 60U);
    unsigned s = (unsigned)(sec % 60U);
    snprintf(buf, len, "%02u:%02u:%02u", h, m, s);
}

/**
 * Hang 2 LCD (toi da ~16 cot): DD/MM/YY HH:MM — man 2.8" 320px.
 * Chua SNTP: chi HH:MM:SS uptime (mot dong ngan).
 */
static void format_datetime_line_for_lcd(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm ti;
    scan_log_wall_tm(now, &ti);
    if (ti.tm_year >= (2020 - 1900)) {
        snprintf(buf, len, "%02u/%02u/%02u %02u:%02u:%02u", (unsigned)ti.tm_mday, (unsigned)(ti.tm_mon + 1),
                 (unsigned)((ti.tm_year + 1900) % 100), (unsigned)ti.tm_hour, (unsigned)ti.tm_min, (unsigned)ti.tm_sec);
    } else {
        format_time_uptime(buf, len);
    }
}



static void uid_to_hex(const mfrc522_uid_t *uid, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    size_t n = uid->size;
    if (n > sizeof(uid->uid_byte)) {
        n = sizeof(uid->uid_byte);
    }
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 2 < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X%s", uid->uid_byte[i],
                                (i + 1 < n) ? ":" : "");
    }
    out[out_len - 1] = '\0';
}

/** UID hex khong dau ':' — ten file tren SD */
static void uid_to_hex_nocolon(const mfrc522_uid_t *uid, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    size_t n = uid->size;
    if (n > sizeof(uid->uid_byte)) {
        n = sizeof(uid->uid_byte);
    }
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 2 < out_len; i++) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X", uid->uid_byte[i]);
    }
    out[out_len - 1] = '\0';
}

/**
 * Kiem tra xem UID nay da quet lan nao trong ngay hom nay chua.
 * Tra ve 1 (Check-in) neu la lan dau, 2 (Check-out) neu la cac lan sau.
 */
static int determine_check_type(const char *uid_nc)
{
    time_t now = time(NULL);
    struct tm ti;
    scan_log_wall_tm(now, &ti);
    
    if (ti.tm_year < (2020 - 1900)) {
        return 0;
    }

    (void)mkdir(BOARD_SD_CHECKIN_DIR, 0777);

    char path[64];
    snprintf(path, sizeof(path), "%s/%02d%02d%02d.txt", BOARD_SD_CHECKIN_DIR, 
             ti.tm_year % 100, ti.tm_mon + 1, ti.tm_mday);

    // 1. Doc file xem UID da ton tai chua
    sd_card_lock();
    FILE *f = fopen(path, "r");
    if (f) {
        char line[32];
        while (fgets(line, sizeof(line), f)) {
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
            line[strcspn(line, "\r\n")] = 0; // xoa ki tu xuong dong
            if (strcmp(line, uid_nc) == 0) {
                fclose(f);
                sd_card_unlock();
                return 2; // Da ton tai -> Check-out
            }
        }
        fclose(f);
    }
    sd_card_unlock();

    // 2. Chua co -> Ghi vao file va tra ve Check-in
    sd_card_lock();
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", uid_nc);
        fclose(f);
    }
    sd_card_unlock();
    return 1; // Lan dau -> Check-in
}

static void trunc_lcd_line(char *s, size_t max_chars)
{
    if (strlen(s) > max_chars) {
        s[max_chars] = '\0';
    }
}

/** Man hinh cho khi khong quet trong IDLE_AFTER_SWIPE_MS */
#define IDLE_LINE1 "Quet Em Di"
#define IDLE_LINE2 "Hihi"
#define IDLE_AFTER_SWIPE_MS 5000u

/** Giong user_rc522.c (project RFID): REQA + doc UID day du (anticollision). */
static void rfid_task(void *arg)
{
    (void)arg;
    char line1[64];
    /** "Ma: " + id[48] toi da ~52 byte — tranh -Wformat-truncation */
    char line_ma[56];
    char dtline[20];
    char last_uid[24];
    char uid_colon[24];
    char uid_nc[32];
    char name[48];
    char id[48];
    bool had_card = false;
    int miss_count = 0; // Bộ đếm chống dội thẻ (debounce)

    last_uid[0] = '\0';
    ESP_LOGI(TAG, "rfid_task bat dau — quet the (log: The <UID> hoac Chua bat duoc the moi ~3s)");
    app_login_pin_init();
    
    /* TWDT: Subscribe task rfid vào watchdog để giám sát treo/lag */
    const bool wdt_ok = (esp_task_wdt_add(NULL) == ESP_OK);

    static uint64_t last_sd_retry_us;
    static bool s_queued_ready_1_wav; /* 1.wav: mot lan sau boot (bo qua lan dau neu vua OTA) */
    uint64_t last_idle_update_us = 0;

    for (;;) {
        if (wdt_ok) {
            esp_task_wdt_reset();
        }
        uint64_t now_us = esp_timer_get_time();

        // if ((now_us - s_last_stack_log_us) >= 15000000ULL) {
        //     s_last_stack_log_us = now_us;
        /* Chu kỳ 1s: cập nhật g_idle_data (màn 0) — **không** gọi LVGL ở đây (chỉ lvgl_task). */
        /* Lần đầu (last_idle_update_us==0): cập nhật ngay — trước đây phải đủ 1s mới có g_idle_data nên đồng hồ đứng tới 1s. */
        if (g_force_ui_update || last_idle_update_us == 0 ||
            (now_us - last_idle_update_us) >= 1000000ULL) {
            last_idle_update_us = now_us;

            time_t now = 0;
            time(&now);
            struct tm ti;
            scan_log_wall_tm(now, &ti);
            int d = 0, mo = 0, y = 0, h = 0, m = 0, s = 0;
            int wday = -1;
            if (ti.tm_year >= (2020 - 1900)) {
                d = ti.tm_mday;
                mo = ti.tm_mon + 1;
                y = ti.tm_year + 1900;
                h = ti.tm_hour;
                m = ti.tm_min;
                s = ti.tm_sec;
                wday = ti.tm_wday;
            } else {
                /* Chưa có NTP hoặc thời gian chưa hợp lệ: dùng uptime cho giờ (tránh đồng hồ đứng 0 và thứ lệch theo epoch 1970). */
                uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
                h = (int)((sec / 3600U) % 24U);
                m = (int)((sec / 60U) % 60U);
                s = (int)(sec % 60U);
            }

            char wifi_st[40] = "Wait...";
#if BOARD_ENABLE_WIFI
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                snprintf(wifi_st, sizeof(wifi_st), "%.*s", (int)sizeof(wifi_st) - 1, (const char *)ap.ssid);
                g_idle_data.rssi = ap.rssi;
            } else {
                strncpy(wifi_st, "Chua ket noi", sizeof(wifi_st) - 1);
                wifi_st[sizeof(wifi_st) - 1] = '\0';
                g_idle_data.rssi = -127;
            }
#else
            strncpy(wifi_st, "WiFi off", sizeof(wifi_st) - 1);
            wifi_st[sizeof(wifi_st) - 1] = '\0';
            g_idle_data.rssi = -127;
#endif

            bool azure_on = app_azure_is_connected();

            if (g_ui_state == 0) {
                snprintf(g_idle_data.wifi_st, sizeof(g_idle_data.wifi_st), "%s", wifi_st);
                g_idle_data.azure_on = azure_on;
                g_idle_data.wday = wday; /* 0=CN … 6=T7; -1 khi chưa có ngày hợp lệ */
                g_idle_data.y = y;
                g_idle_data.mo = mo;
                g_idle_data.d = d;
                g_idle_data.h = h;
                g_idle_data.m = m;
                g_idle_data.s = s;
            }

            /* Chờ kết nối Azure (WiFi ổn định) rồi mới phát nhạc chào mừng để tránh sụt áp nguồn lúc bắt sóng.
             * Nếu sau 30 giây không kết nối được (không có mạng/không dùng Azure), vẫn phát để tránh câm loa. */
            bool ready_to_play_welcome = false;
            if (app_azure_is_connected()) {
                ready_to_play_welcome = true;
            } else if (now_us > 30000000ULL) {
                ready_to_play_welcome = true;
            }

            if (!s_queued_ready_1_wav && sd_card_is_mounted() && ready_to_play_welcome &&
                wifi_portal_time_is_valid()) {
                s_queued_ready_1_wav = true;
#if BOARD_ENABLE_AUDIO && !BOARD_AUDIO_STRESS_TEST
                if (app_ota_take_skip_welcome()) {
                    ESP_LOGI(TAG, "Bo qua 1.wav sau OTA reboot");
                } else {
                    if (BOARD_AUDIO_MS_AFTER_IDLE_PAINT > 0) {
                        vTaskDelay(pdMS_TO_TICKS(BOARD_AUDIO_MS_AFTER_IDLE_PAINT));
                    }
                    if (sd_file_exists(BOARD_SD_AUDIO_1_WAV)) {
                        (void)app_audio_queue_wav(BOARD_SD_AUDIO_1_WAV);
                    } else {
                        ESP_LOGW(TAG, "Thieu %s (tren the: thu muc /audio/, PCM 16-bit mono/stereo)",
                                 BOARD_SD_AUDIO_1_WAV);
                    }
                }
#else
                (void)app_ota_take_skip_welcome();
#endif
            }
        }

        /* Thử mount lại SD mỗi 10 giây (giảm từ 30s để phát hiện thẻ cắm vào nhanh hơn) */
        if (!sd_card_is_mounted() && (now_us - last_sd_retry_us) >= 10ULL * 1000000ULL) {
            last_sd_retry_us = now_us;
            esp_err_t mer = sd_card_mount();
            if (mer == ESP_OK) {
                ESP_LOGI(TAG, "SD mount OK (retry sau khi cam the)");
            }
        }

#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
        sd_card_lock();
#endif
        mfrc522_status_t st = mfrc522_picc_is_new_card_present(mfrc522_spi());
        if (st != MFRC522_OK) {
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
            sd_card_unlock();
#endif
            if (had_card) {
                miss_count++;
                if (miss_count > 2) {
                    had_card = false;
                    last_uid[0] = '\0';
                    miss_count = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* Lấy lại được kết nối thẻ, reset bộ đếm */
        miss_count = 0;

        mfrc522_uid_t uid;
        memset(&uid, 0, sizeof(uid));
        st = mfrc522_picc_read_card_serial(mfrc522_spi(), &uid);
#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_SD
        sd_card_unlock();
#endif
        if (st == MFRC522_TIMEOUT || st == MFRC522_ERROR) {
            /* Tăng delay lên 100ms khi nhàn rỗi để giảm tải CPU và tranh chấp bus */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uid_to_hex(&uid, uid_colon, sizeof(uid_colon));
        uid_to_hex_nocolon(&uid, uid_nc, sizeof(uid_nc));
        if (uid_colon[0] == '\0' || uid_nc[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (!had_card || strcmp(uid_colon, last_uid) != 0) {
#if BOARD_ENABLE_AUDIO && !BOARD_AUDIO_STRESS_TEST
            /* Dừng sạch + xóa hàng đợi — tránh 1.wav/2.wav chồng → "xin xin xin". */
            app_audio_stop_and_clear();
            vTaskDelay(pdMS_TO_TICKS(15));
#endif
            format_datetime_line_for_lcd(dtline, sizeof(dtline));
            memset(name, 0, sizeof(name));
            memset(id, 0, sizeof(id));
            int log_reg = -1;
            int check_type = 0;

            const rfid_time_gate_t time_gate = rfid_time_gate_check();
            if (time_gate != RFID_TIME_GATE_OK) {
                const char *err_msg = (time_gate == RFID_TIME_GATE_WAIT_NTP) ? "Đang lấy thời gian"
                                                                               : "Vui lòng kết nối mạng";
                snprintf(line1, sizeof(line1), "%s", err_msg);
                snprintf(line_ma, sizeof(line_ma), "Ma: -");
                dtline[0] = '\0';
                lv_port_show_swipe_result(line1, line_ma, dtline, false, 0);
                taskYIELD();
                strncpy(last_uid, uid_colon, sizeof(last_uid) - 1);
                last_uid[sizeof(last_uid) - 1] = '\0';
                had_card = true;
                vTaskDelay(pdMS_TO_TICKS(150));
                continue;
            }

            /* --- BLOCK 1: Đọc SD lấy thông tin thẻ --- */
            if (sd_card_is_mounted()) {

                sd_card_lock();
                bool reg = false;
                bool created = false;
                esp_err_t prof_err = card_profile_lookup(uid_nc, name, sizeof(name), id, sizeof(id), &reg, &created);
                if (prof_err == ESP_OK) {
                    log_reg = reg ? 1 : 0;
                    /* Chi the da dang ky moi ghi /checkin/ — the la chi tao profile, khong check-in/out */
                    check_type = reg ? determine_check_type(uid_nc) : 0;
                } else {
                    log_reg = -2;
                }
                sd_card_unlock();

                /* Định dạng văn bản tên và mã */
                if (log_reg >= 0) {
                    if (log_reg == 1) {
                        snprintf(line1, sizeof(line1), "%s", name[0] ? name : "(Chua dat ten)");
                        snprintf(line_ma, sizeof(line_ma), "Ma : %s", id[0] ? id : "-");
                    } else {
                        snprintf(line1, sizeof(line1), "Chua dang ky");
                        snprintf(line_ma, sizeof(line_ma), "Ma: -");
                    }

                    /* Popup LVGL: chỉ chữ (tên + mã + giờ), không hiển thị ảnh */
                    lv_port_show_swipe_result(line1, line_ma, dtline, true, check_type);
                    taskYIELD();

                    /* ĐÃ XÓA sd_png_show_image_at ĐỂ TRÁNH XUNG ĐỘT SPI VỚI LVGL */


                    int32_t msg_idx = app_azure_get_and_increment_msg_index(
                        log_reg == 1 ? MSG_IDX_SWIPE : MSG_IDX_UNKNOWN);

                    /* Ghi log trước khi phát — audio đã dừng ở đầu quẹt thẻ */
                    sd_card_lock();
                    scan_log_append(uid_nc, name, id, log_reg, msg_idx);
                    sd_card_unlock();

                    ESP_LOGI(TAG, "The %s type=%d id=%s | %s | %s", uid_nc, check_type, id, line1, dtline);
                    app_azure_send_telemetry(uid_nc, name, id, log_reg == 1 ? 1 : 0, msg_idx);

#if BOARD_ENABLE_AUDIO
                    {
                        const char *wav_path = (check_type == 1 || check_type == 2) ? BOARD_SD_AUDIO_2_WAV : NULL;
#if !BOARD_AUDIO_STRESS_TEST
                        if (wav_path && BOARD_AUDIO_MS_AFTER_IMAGE > 0) {
                            vTaskDelay(pdMS_TO_TICKS(BOARD_AUDIO_MS_AFTER_IMAGE));
                        }
#endif
#if !BOARD_AUDIO_STRESS_TEST
                        if (wav_path) {
                            sd_card_lock();
                            bool wav_ok = sd_file_exists(wav_path);
                            sd_card_unlock();
                            if (wav_ok) {
                                (void)app_audio_queue_wav(wav_path);
                            }
                        }
#else
                        if (wav_path) {
                            (void)app_audio_queue_wav(wav_path);
                        }
#endif
                    }
#endif
                } else {
                    snprintf(line_ma, sizeof(line_ma), "Ma: -");
                    snprintf(line1, sizeof(line1), "Chua DKy");
                    trunc_lcd_line(line_ma, RFID_UI_MAX_CHARS_PER_LINE);
                    trunc_lcd_line(line1, RFID_UI_MAX_CHARS_PER_LINE);
                    
                    lv_port_show_swipe_result(line1, line_ma, dtline, false, 0);
                    taskYIELD();
                }
            } else {
                snprintf(line_ma, sizeof(line_ma), "Ma: ----");
                snprintf(line1, sizeof(line1), "SD: Err");
                trunc_lcd_line(line_ma, RFID_UI_MAX_CHARS_PER_LINE);
                trunc_lcd_line(line1, RFID_UI_MAX_CHARS_PER_LINE);
                
                lv_port_show_swipe_result(line1, line_ma, dtline, false, 0);
                taskYIELD();
            }

            strncpy(last_uid, uid_colon, sizeof(last_uid) - 1);
            last_uid[sizeof(last_uid) - 1] = '\0';

        }
        had_card = true;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void app_rfid_start(void)
{
    /* Tăng stack lên 16KB để hệ thống đẩy sang PSRAM, giải phóng RAM nội bộ. */
    BaseType_t res = xTaskCreate(rfid_task, "rfid", 16384, NULL, 10, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "L\u1ed7i t\u1ea1o task rfid_task");
    }
}

#endif /* BOARD_RFID_ONLY */
