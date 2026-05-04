#include "app_touch.h"
#include "board_pins.h"
#include "app_rfid.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_wifi.h"
#include "wifi_portal.h"
#include "card_profile.h"
#include "lcd_ui.h"
#include "sd_card.h"
#include <ctype.h>
#include "app_azure.h"
#include "scan_log.h"
#if BOARD_ENABLE_AUDIO
#include "app_audio.h"
#endif

static const char *TAG = "touch";
static spi_device_handle_t s_touch_spi = NULL;

extern int g_ui_state; // 0 = Idle, 1 = Login, 2 = Menu
extern int g_menu_active_item;
extern char g_login_pin[16];
extern bool g_login_error;
extern bool g_force_ui_update;
extern char g_old_pin[16];
extern char g_new_pin[16];
extern int g_pwd_active_field;
extern bool g_pwd_error;
extern int g_log_page;
extern int  g_card_page;
extern char g_edit_uid[20];
extern char g_edit_name[48];
extern char g_edit_id[48];
extern int  g_edit_field;
extern int  g_edit_from_state;
extern int  g_confirm_type;
extern int  g_confirm_from_state;
extern char g_confirm_arg_str[64];
extern char g_confirm_arg_str2[64];
extern void lcd_ui_invalidate_card_cache(void);
extern bool app_login_verify_pin(const char *entered);
extern esp_err_t app_login_save_new_pin(const char *new_pin);

void touch_panel_init(void) {
    if (BOARD_TOUCH_IRQ_GPIO != GPIO_NUM_NC) {
        gpio_set_direction(BOARD_TOUCH_IRQ_GPIO, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BOARD_TOUCH_IRQ_GPIO, GPIO_PULLUP_ONLY);
    }

    if (BOARD_TOUCH_CS_GPIO == GPIO_NUM_NC) return;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,           // 2 MHz cho touch
        .mode = 0,                                
        .spics_io_num = BOARD_TOUCH_CS_GPIO,      
        .queue_size = 3,
    };
    esp_err_t e = spi_bus_add_device(BOARD_LCD_SPI_HOST, &devcfg, &s_touch_spi);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Loi add SPI device cho Touch!");
    } else {
        ESP_LOGI(TAG, "Da khoi tao XPT2046 tren SPI bus!");
    }
}

static int xpt2046_cmd(uint8_t cmd) {
    if (!s_touch_spi) return 0;
    uint8_t rx_data[3] = {0};
    uint8_t tx_data[3] = {cmd, 0x00, 0x00};

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data
    };
    spi_device_transmit(s_touch_spi, &t);
    return (rx_data[1] << 8 | rx_data[2]) >> 3; 
}

bool touch_panel_read(int *x, int *y) {
    if (!s_touch_spi) return false;
    if (BOARD_TOUCH_IRQ_GPIO != GPIO_NUM_NC) {
        if (gpio_get_level(BOARD_TOUCH_IRQ_GPIO) != 0) {
            return false;
        }
    }

    int z1 = xpt2046_cmd(0xB0);
    int z2 = xpt2046_cmd(0xC0);
    int z = z1 + 4095 - z2;
    if (z < 300) return false;

    int sum_x = 0, sum_y = 0;
    int samples = 5;
    for (int i = 0; i < samples; i++) {
        int cx = xpt2046_cmd(0xD0);
        int cy = xpt2046_cmd(0x90);
        if (cx == 4095 || cy == 4095) return false; // Noi bo nhiễu khi thả tay
        sum_x += cx;
        sum_y += cy;
    }
    int raw_x = sum_x / samples;
    int raw_y = sum_y / samples;

    // Calibration
    int min_x = 300, max_x = 3800;
    int min_y = 300, max_y = 3800;
    
    // Gán toạ độ
    *x = (raw_x - min_x) * BOARD_LCD_H_RES / (max_x - min_x);
    *y = (raw_y - min_y) * BOARD_LCD_V_RES / (max_y - min_y);
    
    // Đảo ngược trục Y và trục X
    *x = BOARD_LCD_H_RES - *x;
    *y = BOARD_LCD_V_RES - *y; 
    
    if (*x < 0) { *x = 0; } 
    if (*x >= BOARD_LCD_H_RES) { *x = BOARD_LCD_H_RES - 1; }
    if (*y < 0) { *y = 0; } 
    if (*y >= BOARD_LCD_V_RES) { *y = BOARD_LCD_V_RES - 1; }

    // Log the RAW values to help with manual calibration if it's still slightly off
    // ESP_LOGI(TAG, "Raw X: %d, Raw Y: %d -> Px: %d, Py: %d", raw_x, raw_y, *x, *y);

    return true;
}

static void touch_task(void *arg) {
    int x, y;
    bool is_pressed = false;

    while (1) {
        if (touch_panel_read(&x, &y)) {
            if (!is_pressed) {
                is_pressed = true;
                ESP_LOGI(TAG, "Touch Px: %d, Py: %d", x, y);
                bool did_action = false;
                
                // Kiem tra UI State hien tai
                if (g_ui_state == 0) {
                    // IDLE Screen: Kiem tra nut "DANG NHAP" (y: 400->450)
                    if (y >= 380 && y <= 470) {
                        ESP_LOGI(TAG, "Mo man hinh dang nhap");
                        memset(g_login_pin, 0, sizeof(g_login_pin));
                        g_login_error = false;
                        g_ui_state = 1; // Chuyen sang Login
                        did_action = true;
                    }
                } else if (g_ui_state == 1) {
                    // Numpad Login (3 cot x 4 hang + nut OK rieng)
                    // LCD ve: start_y=150, key_w=80, key_h=55, sp_x=15, sp_y=10, start_x=25
                    int np_start_y = 150;
                    int np_key_w   = 80;
                    int np_key_h   = 55;
                    int np_sp_x    = 15;
                    int np_sp_y    = 10;
                    int np_start_x = 25;
                    int ok_y_top   = 410;
                    int ok_y_bot   = 470;

                    const char* kmap_np[4][3] = {
                        {"1","2","3"},
                        {"4","5","6"},
                        {"7","8","9"},
                        {"Huy","0","<"}
                    };

                    if (y >= np_start_y && y < np_start_y + 4 * (np_key_h + np_sp_y)) {
                        int row = (y - np_start_y) / (np_key_h + np_sp_y);
                        if (row >= 0 && row < 4) {
                            int by = np_start_y + row * (np_key_h + np_sp_y);
                            if (y >= by && y < by + np_key_h) {
                                for (int c = 0; c < 3; c++) {
                                    int bx = np_start_x + c * (np_key_w + np_sp_x);
                                    if (x >= bx && x < bx + np_key_w) {
                                        const char* key = kmap_np[row][c];
                                        ESP_LOGI(TAG, "Bam numpad: %s", key);
                                        g_login_error = false;
                                        if (strcmp(key, "<") == 0) {
                                            int len = strlen(g_login_pin);
                                            if (len > 0) g_login_pin[len - 1] = '\0';
                                        } else if (strcmp(key, "Huy") == 0) {
                                            g_ui_state = 0;
                                        } else {
                                            int len = strlen(g_login_pin);
                                            if (len < 15) { g_login_pin[len] = key[0]; g_login_pin[len+1] = '\0'; }
                                        }
                                        did_action = true;
                                        break;
                                    }
                                }
                            }
                        }
                    } else if (y >= ok_y_top && y <= ok_y_bot) {
                        // Nut OK lon
                        ESP_LOGI(TAG, "Bam OK");
                        if (app_login_verify_pin(g_login_pin)) {
                            g_ui_state = 2;
                        } else {
                            g_login_error = true;
                            memset(g_login_pin, 0, sizeof(g_login_pin));
                        }
                        did_action = true;
                    }
                } else if (g_ui_state == 2) {
                    // 2x2 Grid Menu
                    if (y >= 80 && y <= 210) {
                        if (x >= 20 && x <= 150) {
                            ESP_LOGI(TAG, "Chon: CAI DAT");
                            g_menu_active_item = 0;
                            g_ui_state = 3;
                            did_action = true;
                        } else if (x >= 170 && x <= 300) {
                            ESP_LOGI(TAG, "Chon: NHAT KY");
                            g_menu_active_item = 1;
                            g_ui_state = 8; // Nhat ky
                            g_log_page = 0;
                            did_action = true;
                        }
                    } else if (y >= 230 && y <= 360) {
                        if (x >= 20 && x <= 150) {
                            ESP_LOGI(TAG, "Chon: DOI THE");
                            g_menu_active_item = 2;
                            g_card_page = 0;
                            g_ui_state = 9;
                            did_action = true;
                        } else if (x >= 170 && x <= 300) {
                            ESP_LOGI(TAG, "Chon: THEM MOI");
                            g_menu_active_item = 3;
                            g_card_page = 0;
                            g_ui_state = 10;
                            did_action = true;
                        }
                    } else if (y >= 390 && y <= 450) {
                        ESP_LOGI(TAG, "Chon: QUAY LAI");
                        g_menu_active_item = 4;
                        g_ui_state = 0; // Ve lai Idle
                        did_action = true;
                    }
                } else if (g_ui_state == 3) {
                    // Settings Menu
                    if (y >= 100 && y <= 160) {
                        ESP_LOGI(TAG, "Chon: QUAN LY WIFI");
                        g_menu_active_item = 0;
                        g_ui_state = 5; // WiFi Config
                        did_action = true;
                    } else if (y >= 180 && y <= 240) {
                        ESP_LOGI(TAG, "Chon: DOI MAT KHAU");
                        g_menu_active_item = 1;
                        g_ui_state = 4; // Change password
                        memset(g_old_pin, 0, sizeof(g_old_pin));
                        memset(g_new_pin, 0, sizeof(g_new_pin));
                        g_pwd_active_field = 0;
                        g_pwd_error = false;
                        did_action = true;
                    } else if (y >= 260 && y <= 320) {
                        ESP_LOGI(TAG, "Chon: QUAY LAI (Tu Cai Dat)");
                        g_menu_active_item = 2;
                        g_ui_state = 2; // Ve Grid Menu
                        did_action = true;
                    }
                } else if (g_ui_state == 4) {
                    // Numpad Doi Mat Khau
                    // Gia tri LCD: np_start_y=185, key_h=52, sp_y=8 -> step=60
                    //              np_key_w=80, sp_x=15, start_x=25
                    //              nut LUU: y=433..473
                    int np4_key_h = 52;
                    int np4_step  = 60;  // 52+8
                    int np4_ok_y  = 433;
                    int np4_ok_h  = 40;

                    if (y >= 70 && y <= 115) {
                        g_pwd_active_field = 0;
                        did_action = true;
                    } else if (y >= 130 && y <= 175) {
                        g_pwd_active_field = 1;
                        did_action = true;
                    } else if (y >= 185 && y < 185 + 4 * np4_step) {
                        int row = (y - 185) / np4_step;
                        if (row >= 0 && row < 4) {
                            int by = 185 + row * np4_step;
                            if (y >= by && y < by + np4_key_h) {
                                const char* kmap_np4[4][3] = {
                                    {"1","2","3"},
                                    {"4","5","6"},
                                    {"7","8","9"},
                                    {"Huy","0","<"}
                                };
                                for (int c = 0; c < 3; c++) {
                                    int bx = 25 + c * (80 + 15);
                                    if (x >= bx && x < bx + 80) {
                                        const char* key = kmap_np4[row][c];
                                        char* cur = (g_pwd_active_field == 0) ? g_old_pin : g_new_pin;
                                        g_pwd_error = false;
                                        if (strcmp(key, "<") == 0) {
                                            int len = strlen(cur);
                                            if (len > 0) cur[len-1] = '\0';
                                        } else if (strcmp(key, "Huy") == 0) {
                                            g_ui_state = 3;
                                        } else {
                                            int len = strlen(cur);
                                            if (len < 15) { cur[len] = key[0]; cur[len+1] = '\0'; }
                                        }
                                        did_action = true;
                                        break;
                                    }
                                }
                            }
                        }
                    } else if (y >= np4_ok_y && y <= np4_ok_y + np4_ok_h) {
                        // Nut LUU — hien bang xac nhan doi mat khau
                        if (strlen(g_new_pin) == 0) {
                            g_pwd_error = true;
                        } else if (!app_login_verify_pin(g_old_pin)) {
                            g_pwd_error = true;
                            memset(g_old_pin, 0, sizeof(g_old_pin));
                            g_pwd_active_field = 0;
                        } else {
                            g_confirm_type = 5;
                            g_confirm_from_state = 4;
                            g_ui_state = 12;
                        }
                        did_action = true;
                    }
                } else if (g_ui_state == 9 || g_ui_state == 10) {
                    // Danh sach the / the la — layout giong log screen
                    // Header: 0-47, col_hdr: 48-63, rows: 64+ri*58, nav: 354-389, back: 436-476
                    int rs    = 64;  // row start
                    int rh    = 58;  // row height
                    int nav_y = rs + 5 * rh;  // 354
                    int bck_y = 436;
                    if (y >= bck_y && y <= bck_y + 40) {
                        g_ui_state = 2; // Quay lai menu
                        did_action = true;
                    } else if (y <= 80 && g_card_page > 0) {
                        g_card_page--;  did_action = true;
                    } else if (y >= nav_y && y < nav_y + 36) {
                        if (x < 160) {
                            if (g_card_page > 0) { g_card_page--; did_action = true; }
                        } else {
                            int total = lcd_ui_card_list_total();
                            int maxp = (total <= 0) ? 0 : (total - 1) / CARD_PROFILE_PAGE_ROWS;
                            if (g_card_page < maxp) {
                                g_card_page++;
                                did_action = true;
                            }
                        }
                    } else if (y >= rs && y < nav_y) {
                        int ri = (y - rs) / rh;
                        if (ri >= 0 && ri < CARD_PROFILE_PAGE_ROWS) {
                            static CardProfileEntry_t s_touch_card_row[CARD_PROFILE_PAGE_ROWS];
                            bool only_u = (g_ui_state == 10);
                            int n = card_profile_list_page(s_touch_card_row, CARD_PROFILE_PAGE_ROWS, only_u,
                                                           g_card_page * CARD_PROFILE_PAGE_ROWS);
                            if (ri >= n) {
                                did_action = true;
                            } else if (x >= 288) {
                                g_confirm_type = 1;
                                g_confirm_from_state = g_ui_state;
                                strncpy(g_confirm_arg_str, s_touch_card_row[ri].uid, sizeof(g_confirm_arg_str)-1);
                                strncpy(g_confirm_arg_str2, s_touch_card_row[ri].name, sizeof(g_confirm_arg_str2)-1);
                                g_ui_state = 12;
                                did_action = true;
                            } else {
                                strncpy(g_edit_uid,  s_touch_card_row[ri].uid,  sizeof(g_edit_uid)-1);
                                strncpy(g_edit_name, s_touch_card_row[ri].name, sizeof(g_edit_name)-1);
                                strncpy(g_edit_id,   s_touch_card_row[ri].id,   sizeof(g_edit_id)-1);
                                g_edit_field      = 0;
                                g_edit_from_state = g_ui_state;
                                g_soft_kb_upper = false;
                                g_soft_kb_sym_page = 0;
                                g_ui_state = 11;
                                did_action = true;
                            }
                        }
                    }
                } else if (g_ui_state == 11) {
                    /* Form sua the — layout giong mk WiFi (qy=168, 6 hang + SYM) */
                    int qy = 168, qkey_h = 42, qstep = 50;
                    int num_keys[6] = {10, 10, 9, 9, 10, 4};
                    int start_x[6] = {10, 10, 25, 10, 10, 11};
                    int key_w[6] = {28, 28, 28, 28, 28, 70};
                    int sp_x[6] = {2, 2, 2, 2, 2, 6};
                    const char *km_row[4][10] = {
                        {"1","2","3","4","5","6","7","8","9","0"},
                        {"Q","W","E","R","T","Y","U","I","O","P"},
                        {"A","S","D","F","G","H","J","K","L",""},
                        {"^","Z","X","C","V","B","N","M","<",""},
                    };
                    if (y >= 56 && y <= 104) {
                        g_edit_field = 0;
                        did_action = true;
                    } else if (y >= 114 && y <= 162) {
                        g_edit_field = 1;
                        did_action = true;
                    } else if (y >= qy && y <= qy + 6 * qstep) {
                        int row = (y - qy) / qstep;
                        int by = qy + row * qstep;
                        if (row >= 0 && row <= 5 && y >= by && y < by + qkey_h) {
                            for (int c = 0; c < num_keys[row]; c++) {
                                int bx = start_x[row] + c * (key_w[row] + sp_x[row]);
                                if (x < bx || x >= bx + key_w[row]) {
                                    continue;
                                }
                                char *cur = (g_edit_field == 0) ? g_edit_name : g_edit_id;
                                const size_t maxlen = 47;
                                if (row == 5) {
                                    if (c == 0) {
                                        g_ui_state = g_edit_from_state;
                                    } else if (c == 1) {
                                        g_soft_kb_sym_page = (g_soft_kb_sym_page + 1) % SOFT_KB_SYM_PAGES;
                                    } else if (c == 2) {
                                        int l = (int)strlen(cur);
                                        if ((size_t)l < maxlen) {
                                            cur[l] = ' ';
                                            cur[l + 1] = '\0';
                                        }
                                    } else if (c == 3) {
                                        g_confirm_type = 2;
                                        g_confirm_from_state = 11;
                                        g_ui_state = 12;
                                    }
                                } else if (row == 4) {
                                    uint8_t ch = g_soft_sym_chars[g_soft_kb_sym_page][c];
                                    if (ch != 0) {
                                        int l = (int)strlen(cur);
                                        if ((size_t)l < maxlen) {
                                            cur[l] = (char)ch;
                                            cur[l + 1] = '\0';
                                        }
                                    }
                                } else if (row == 3) {
                                    const char *key = km_row[3][c];
                                    if (strcmp(key, "<") == 0) {
                                        int l = (int)strlen(cur);
                                        if (l > 0) {
                                            cur[l - 1] = '\0';
                                        }
                                    } else if (strcmp(key, "^") == 0 && c == 0) {
                                        g_soft_kb_upper = !g_soft_kb_upper;
                                    } else if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                                        int l = (int)strlen(cur);
                                        if ((size_t)l < maxlen) {
                                            char ch = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                                            cur[l] = ch;
                                            cur[l + 1] = '\0';
                                        }
                                    }
                                } else if (row == 2 || row == 1) {
                                    const char *key = km_row[row][c];
                                    if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                                        int l = (int)strlen(cur);
                                        if ((size_t)l < maxlen) {
                                            char ch = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                                            cur[l] = ch;
                                            cur[l + 1] = '\0';
                                        }
                                    }
                                } else if (row == 0) {
                                    const char *key = km_row[0][c];
                                    if (key[0] != '\0' && key[1] == '\0') {
                                        int l = (int)strlen(cur);
                                        if ((size_t)l < maxlen) {
                                            cur[l] = key[0];
                                            cur[l + 1] = '\0';
                                        }
                                    }
                                }
                                did_action = true;
                                break;
                            }
                        }
                    }
                } else if (g_ui_state == 5 || g_ui_state == 6) {
                    if (y >= 390 && y <= 450) {
                        g_ui_state = (g_ui_state == 5) ? 3 : 5;
                        did_action = true;
                    } else if (y >= 70 && y <= 345) {
                        int i = (y - 70) / 55;
                        int display_cnt = (g_ui_state == 5) ? wifi_list_get_count() : g_wifi_scan_count;
                        if (display_cnt > 5) display_cnt = 5;
                        
                        if (i < display_cnt) {
                            if (g_ui_state == 5 && x >= 240) { // Nut Xoa
                                char ssid[33];
                                wifi_list_get_item(i, ssid, NULL);
                                g_confirm_type = 3;
                                g_confirm_from_state = 5;
                                strncpy(g_confirm_arg_str, ssid, sizeof(g_confirm_arg_str)-1);
                                g_ui_state = 12;
                            } else if (g_ui_state == 6) { // Chon mang tu scan
                                strncpy(g_wifi_selected_ssid, g_wifi_scan_res[i], 32);
                                memset(g_wifi_entered_pass, 0, sizeof(g_wifi_entered_pass));
                                g_soft_kb_upper = false;
                                g_soft_kb_sym_page = 0;
                                g_ui_state = 7;
                            }
                        } else if (i == display_cnt && g_ui_state == 5) {
                            // QUET THEM
                            wifi_scan_config_t scan_config = {.ssid = 0, .bssid = 0, .channel = 0, .show_hidden = false};
                            esp_wifi_scan_start(&scan_config, true);
                            uint16_t ap_count = 0;
                            esp_wifi_scan_get_ap_num(&ap_count);
                            if (ap_count > 0) {
                                wifi_ap_record_t *aps = malloc(ap_count * sizeof(wifi_ap_record_t));
                                if (aps) {
                                    esp_wifi_scan_get_ap_records(&ap_count, aps);
                                    g_wifi_scan_count = ap_count;
                                    if (g_wifi_scan_count > 5) g_wifi_scan_count = 5;
                                    for(int j=0; j<g_wifi_scan_count; j++) {
                                        strncpy(g_wifi_scan_res[j], (char*)aps[j].ssid, 32);
                                    }
                                    free(aps);
                                }
                            } else {
                                g_wifi_scan_count = 0;
                            }
                            g_ui_state = 6;
                        }
                        did_action = true;
                    }
                } else if (g_ui_state == 7) {
                    int qw_y = 150, qw7_key_h = 42, qw7_step = 50;
                    int num_keys[6] = {10, 10, 9, 9, 10, 4};
                    int start_x[6] = {10, 10, 25, 10, 10, 11};
                    int key_w[6] = {28, 28, 28, 28, 28, 70};
                    int sp_x[6] = {2, 2, 2, 2, 2, 6};
                    const char *km_row[4][10] = {
                        {"1","2","3","4","5","6","7","8","9","0"},
                        {"Q","W","E","R","T","Y","U","I","O","P"},
                        {"A","S","D","F","G","H","J","K","L",""},
                        {"^","Z","X","C","V","B","N","M","<",""},
                    };
                    if (y >= qw_y && y <= qw_y + 6 * qw7_step) {
                        int row = (y - qw_y) / qw7_step;
                        int by = qw_y + row * qw7_step;
                        if (row >= 0 && row <= 5 && y >= by && y < by + qw7_key_h) {
                            for (int c = 0; c < num_keys[row]; c++) {
                                int bx = start_x[row] + c * (key_w[row] + sp_x[row]);
                                if (x < bx || x >= bx + key_w[row]) {
                                    continue;
                                }
                                if (row == 5) {
                                    if (c == 0) {
                                        g_ui_state = 6;
                                    } else if (c == 1) {
                                        g_soft_kb_sym_page = (g_soft_kb_sym_page + 1) % SOFT_KB_SYM_PAGES;
                                    } else if (c == 2) {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len < 63) {
                                            g_wifi_entered_pass[len] = ' ';
                                            g_wifi_entered_pass[len + 1] = '\0';
                                        }
                                    } else if (c == 3) {
                                        g_confirm_type = 4;
                                        g_confirm_from_state = 7;
                                        g_ui_state = 12;
                                    }
                                } else if (row == 4) {
                                    uint8_t ch = g_soft_sym_chars[g_soft_kb_sym_page][c];
                                    if (ch != 0) {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len < 63) {
                                            g_wifi_entered_pass[len] = (char)ch;
                                            g_wifi_entered_pass[len + 1] = '\0';
                                        }
                                    }
                                } else if (row == 3) {
                                    const char *key = km_row[3][c];
                                    if (strcmp(key, "<") == 0) {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len > 0) {
                                            g_wifi_entered_pass[len - 1] = '\0';
                                        }
                                    } else if (strcmp(key, "^") == 0 && c == 0) {
                                        g_soft_kb_upper = !g_soft_kb_upper;
                                    } else if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len < 63) {
                                            char ch = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                                            g_wifi_entered_pass[len] = ch;
                                            g_wifi_entered_pass[len + 1] = '\0';
                                        }
                                    }
                                } else if (row == 2 || row == 1) {
                                    const char *key = km_row[row][c];
                                    if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len < 63) {
                                            char ch = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                                            g_wifi_entered_pass[len] = ch;
                                            g_wifi_entered_pass[len + 1] = '\0';
                                        }
                                    }
                                } else if (row == 0) {
                                    const char *key = km_row[0][c];
                                    if (key[0] != '\0' && key[1] == '\0') {
                                        int len = (int)strlen(g_wifi_entered_pass);
                                        if (len < 63) {
                                            g_wifi_entered_pass[len] = key[0];
                                            g_wifi_entered_pass[len + 1] = '\0';
                                        }
                                    }
                                }
                                did_action = true;
                                break;
                            }
                        }
                    }
                } else if (g_ui_state == 8) {
                    // Nhat ky — cuon trang
                    if (y >= 430 && y <= 478) {
                        g_ui_state = 2; // Quay lai menu
                        did_action = true;
                    } else if (y <= 80 && g_log_page > 0) {
                        g_log_page--;   // Trang truoc
                        did_action = true;
                    } else if (y >= 380 && y < 430) {
                        g_log_page++;   // Trang sau
                        did_action = true;
                    }
                } else if (g_ui_state == 12) {
                    // Confirm Screen (HUY: 30-140, DONG Y: 180-290, y: 280-350)
                    if (y >= 280 && y <= 350) {
                        if (x >= 30 && x <= 140) { // HUY
                            g_ui_state = g_confirm_from_state;
                            did_action = true;
                        } else if (x >= 180 && x <= 290) { // DONG Y
                            bool action_ok = false;
                            if (g_confirm_type == 1) { // XOA THE
                                card_profile_delete(g_confirm_arg_str);
                                app_azure_send_card_event(g_confirm_arg_str, g_confirm_arg_str2, "", 604);
                                scan_log_append_admin(g_confirm_arg_str, g_confirm_arg_str2, "", "DEL");
                                lcd_ui_invalidate_card_cache();
                                g_ui_state = g_confirm_from_state;
                                action_ok = true;
                            } else if (g_confirm_type == 2) { // LUU THE
                                card_profile_save(g_edit_uid, g_edit_name, g_edit_id);
                                app_azure_send_card_event(g_edit_uid, g_edit_name, g_edit_id, 603);
                                scan_log_append_admin(g_edit_uid, g_edit_name, g_edit_id, "SAVE");
                                lcd_ui_invalidate_card_cache();
                                g_ui_state = g_edit_from_state;
                                action_ok = true;
                            } else if (g_confirm_type == 3) { // XOA WIFI
                                wifi_list_remove(g_confirm_arg_str);
                                g_ui_state = g_confirm_from_state;
                                action_ok = true;
                            } else if (g_confirm_type == 4) { // LUU WIFI
                                wifi_list_add(g_wifi_selected_ssid, g_wifi_entered_pass);
                                g_ui_state = 5;
                                action_ok = true;
                            } else if (g_confirm_type == 5) { // DOI MAT KHAU
                                if (app_login_save_new_pin(g_new_pin) != ESP_OK) {
                                    ESP_LOGW(TAG, "Luu PIN NVS that bai");
                                    g_pwd_error = true;
                                    g_ui_state = 4;
                                } else {
                                    ESP_LOGI(TAG, "Da luu mat khau moi vao NVS");
                                    memset(g_old_pin, 0, sizeof(g_old_pin));
                                    memset(g_new_pin, 0, sizeof(g_new_pin));
                                    g_ui_state = 3;
                                    action_ok = true;
                                }
                            }
#if BOARD_ENABLE_AUDIO
                            if (action_ok) {
                                (void)app_audio_queue_wav(BOARD_SD_AUDIO_4_WAV);
                            }
#else
                            (void)action_ok;
#endif
                            did_action = true;
                        }
                    }
                }
                if (did_action) g_force_ui_update = true;
            } // end if (!is_pressed)
        } else {
            is_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_touch_start(void) {
    touch_panel_init();
    xTaskCreate(touch_task, "touch_task", 8192, NULL, 5, NULL);
}
