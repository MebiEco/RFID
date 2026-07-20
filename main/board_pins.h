#pragma once

/**
 * =============================================================================
 * CẤU HÌNH PHẦN CỨNG — CHỈ SỬA GPIO TẠI ĐÂY CHO KHỚP MẠCH ĐÃ HÀN.
 * SD = SPI3 (CS=5…); LCD SPI2 — 2.8" 320×240 (profile: board_lcd_panel.h / menuconfig).
 * =============================================================================
 */

#include "driver/gpio.h"
#include "hal/spi_types.h"
#include "board_lcd_panel.h"

/**
 * 1 = chỉ thẻ SD; 0 = không ép chế độ SD-only.
 * BOARD_RFID_ONLY 1 = chỉ RC522 bit-bang (log UID), không mount SD.
 * Mặc định 0: SD SPI3 (21/47/48 + CS 5) + RC522 **tách bus** bit-bang 11/12/13.
 */
#ifndef BOARD_SD_ONLY
#define BOARD_SD_ONLY 0
#endif

#ifndef BOARD_RFID_ONLY
#define BOARD_RFID_ONLY 0
#endif

#if BOARD_SD_ONLY
#undef BOARD_RFID_ONLY
#define BOARD_RFID_ONLY 0
#define BOARD_ENABLE_SD 1
#define BOARD_ENABLE_LCD 0
#define BOARD_ENABLE_WIFI 0
#define BOARD_ENABLE_AZURE 0
#define BOARD_ENABLE_AUDIO 0
#define BOARD_ENABLE_RFID 0
#elif BOARD_RFID_ONLY
#define BOARD_ENABLE_LCD 0
#define BOARD_ENABLE_WIFI 0
#define BOARD_ENABLE_SD 0
#define BOARD_ENABLE_AZURE 0
#define BOARD_ENABLE_RFID 1
/**
 * Chỉ RC522: bit-bang GPIO (SCK/MOSI/MISO=11/12/13, CS=4, RST=3) — tránh lỗi
 * SPI master/DMA/CS trên bus; dễ đọc thẻ hơn khi thử nghiệm. Muốn lại HW SPI2:
 * #undef khi build hoặc sửa tạm thành 1 (kém ổn hơn nếu vẫn VersionReg=0x00).
 */
#define BOARD_RC522_SHARE_SD_SPI_BUS 0
#else
/**
 * Kiosk (mặc định): **ba đường tách** — không tranh `spi_bus` trong IDF:
 *   - LCD → SPI2 2.8" ngang (39 CLK, 40 SI, 9 DC, 10 CS, 8 RES); profile: menuconfig / board_lcd_panel.h.
 * Chỉ “chung SPI” khi bật BOARD_RC522_SHARE_SD_SPI_BUS (RC522+SD cùng SPI3, hai CS) hoặc BOARD_LCD_SHARE_SPI2_WITH_SD.
 * Quẹt thẻ → tra profile/checkin trên SD. Muốn RC522 chung SPI với SD: #define BOARD_RC522_SHARE_SD_SPI_BUS 1 trước include.
 * Mặc định bật WiFi portal + Azure MQTT; tắt: BOARD_ENABLE_WIFI / BOARD_ENABLE_AZURE 0 trước include.
 */
#define BOARD_RC522_SHARE_SD_SPI_BUS 0
#ifndef BOARD_ENABLE_LCD
#define BOARD_ENABLE_LCD 1
#endif
#ifndef BOARD_ENABLE_WIFI
#define BOARD_ENABLE_WIFI 1
#endif
#ifndef BOARD_ENABLE_SD
#define BOARD_ENABLE_SD 1
#endif
#ifndef BOARD_ENABLE_AZURE
#define BOARD_ENABLE_AZURE 1
#endif
#ifndef BOARD_ENABLE_RFID
#define BOARD_ENABLE_RFID 1
#endif
#ifndef BOARD_ENABLE_AUDIO
#define BOARD_ENABLE_AUDIO 1
#endif
#endif

/** FAT (khi dùng lại tính năng đọc thẻ nhớ) */
#define BOARD_SD_MOUNT_POINT "/sdcard"
/** 1.wav = sau khi bắt WiFi + đã NTP; 2/3 = IN/OUT theo /sdcard/checkin/ (app_rfid) */
#define BOARD_SD_AUDIO_DIR     BOARD_SD_MOUNT_POINT "/audio"
#define BOARD_SD_AUDIO_1_WAV  BOARD_SD_AUDIO_DIR "/1.wav"
#define BOARD_SD_AUDIO_2_WAV  BOARD_SD_AUDIO_DIR "/2.wav"
#define BOARD_SD_AUDIO_3_WAV  BOARD_SD_AUDIO_DIR "/3.wav"
/** 4.wav = am thanh xac nhan (luu/xoa thanh cong tren man hinh) */
#define BOARD_SD_AUDIO_4_WAV  BOARD_SD_AUDIO_DIR "/4.wav"
#define BOARD_SD_CHECKIN_DIR  BOARD_SD_MOUNT_POINT "/checkin"
/**
 * File profile theo UID: /sdcard/profiles/<UID>.txt (giong cau truc thu muc /audio/).
 * Co the ghi de bang may tinh trong thu muc nay.
 */
#ifndef BOARD_SD_PROFILES_DIR
#define BOARD_SD_PROFILES_DIR BOARD_SD_MOUNT_POINT "/profiles"
#endif
/** Anh mac dinh khi khong co <UID>.jpg — uu tien trong profiles/, roi goc the. */
#ifndef BOARD_SD_PROFILES_DEFAULT_JPG
#define BOARD_SD_PROFILES_DEFAULT_JPG BOARD_SD_PROFILES_DIR "/default.jpg"
#endif
#ifndef BOARD_SD_ROOT_DEFAULT_JPG
#define BOARD_SD_ROOT_DEFAULT_JPG BOARD_SD_MOUNT_POINT "/default.jpg"
#endif
#define BOARD_SD_IMAGE_FILENAME "a.jpg"
#define BOARD_SD_IMAGE_PATH BOARD_SD_MOUNT_POINT "/" BOARD_SD_IMAGE_FILENAME

/**
 * Lật ảnh JPEG theo chiều dọc / chiều ngang nếu ảnh hiển thị bị ngược.
 * Sửa thành 1 để lật, 0 để bình thường.
 */
#ifndef BOARD_SD_IMAGE_FLIP_Y
#define BOARD_SD_IMAGE_FLIP_Y 0
#endif
#ifndef BOARD_SD_IMAGE_FLIP_X
#define BOARD_SD_IMAGE_FLIP_X 0
#endif

/**
 * Nhat ky quet the (append), doc tren web /scans.
 * FATFS mac dinh LFN_NONE: ten file phai 8.3 (toi da 8 ky tu ten + .ext).
 * Khong dung rfid_scans.csv (9 ky tu) — fopen bao EINVAL.
 */
#ifndef BOARD_SD_RFID_LOG_PATH
#define BOARD_SD_RFID_LOG_PATH BOARD_SD_MOUNT_POINT "/rfid_log.csv"
#endif

/**
 * Lệch giờ so với UTC (giây) cho timestamp CSV và trang /scans.
 * Dùng cộng UTC + offset rồi gmtime_r — tránh localtime_r (tzset/getenv) hay
 * crash trên task HTTP. Việt Nam (ICT): +7 giờ.
 */
#ifndef BOARD_LOCAL_UTC_OFFSET_SEC
#define BOARD_LOCAL_UTC_OFFSET_SEC (7 * 3600)
#endif

/**
 * 1: RC522 cùng **SPI3** với SD (chung SCK/MOSI/MISO với SD), CS RC522=4 / CS thẻ=5.
 * 0: RC522 bit-bang 11/12/13 — **không** dùng SPI hardware chung với SD (mặc định kiosk).
 */
#ifndef BOARD_RC522_SHARE_SD_SPI_BUS
#define BOARD_RC522_SHARE_SD_SPI_BUS 0
#endif
#ifndef RC522_SPI_CLOCK_HZ
/** MFRC522 thường ổn ≤~4 MHz; 500 kHz an toàn bus chung SD + nhiễu dây. */
#define RC522_SPI_CLOCK_HZ (500000)
#endif

/**
 * SD: SPI3 — SCK=21, MOSI(SI)=47, MISO(SO)=48, CS=5. LCD dùng SPI2 riêng.
 */
#define BOARD_SD_SPI_HOST SPI3_HOST

#define BOARD_SD_CS_GPIO GPIO_NUM_5
#define BOARD_SD_SCK_GPIO GPIO_NUM_21
#define BOARD_SD_MOSI_GPIO GPIO_NUM_47
#define BOARD_SD_MISO_GPIO GPIO_NUM_48

/**
 * Tốc độ SPI cho SD (kHz).
 * 10 MHz dễ CRC/MISO=FF trên dây dài hoặc khi WiFi phát cao — 4 MHz thường ổn hơn.
 */
#ifndef BOARD_SD_SPI_MAX_FREQ_KHZ
#define BOARD_SD_SPI_MAX_FREQ_KHZ 2000
#endif
/** Chờ sau khi CS cao + bus init trước mount (ms); WiFi/MQTT bật → nguồn dao động: tăng nếu vẫn CRC. */
#ifndef BOARD_SD_PRE_MOUNT_DELAY_MS
#define BOARD_SD_PRE_MOUNT_DELAY_MS 500
#endif

/** 0 = không màn PIN / menu cài đặt (chỉ idle + quẹt thẻ). */
#ifndef BOARD_ENABLE_LOGIN
#define BOARD_ENABLE_LOGIN 0
#endif

#define BOARD_LCD_SPI_HOST SPI2_HOST
/** ESP32-S3: OK. ESP32 cổ điển: GPIO34–39 chỉ INPUT — không thể làm SCK/MOSI. */
#define BOARD_LCD_SCK_GPIO GPIO_NUM_39
#define BOARD_LCD_MOSI_GPIO GPIO_NUM_40
#define BOARD_LCD_MISO_GPIO GPIO_NUM_41
#define BOARD_LCD_DC_GPIO GPIO_NUM_9
/** Module 2.8" không có CS (nối sẵn GND): ESP vẫn khai CS cho driver SPI. */
#define BOARD_LCD_CS_GPIO GPIO_NUM_10
#define BOARD_LCD_RST_GPIO GPIO_NUM_8

#ifndef BOARD_LCD_SHARE_SPI2_WITH_SD
#define BOARD_LCD_SHARE_SPI2_WITH_SD 0
#endif

/** Hàng đợi SPI color (esp_lcd); 2 đủ khi vẽ đồng bộ — tránh queue full + WiFi. */
#ifndef BOARD_LCD_SPI_TRANS_QUEUE_DEPTH
#define BOARD_LCD_SPI_TRANS_QUEUE_DEPTH 2
#endif

#define BOARD_LCD_BL_GPIO GPIO_NUM_NC
#define BOARD_LCD_BL_ACTIVE_HIGH 1
#define BOARD_LCD_SPI_USE_MISO 0
#define BOARD_LCD_H_RES 320
#define BOARD_LCD_V_RES 240
#define BOARD_LCD_USE_ILI9341 1
#define BOARD_LCD_ENABLE_TOUCH 0
#define BOARD_LCD_COLOR_PIPELINE_BGR 1
#define BOARD_LCD_SWAP_XY_AFTER_INIT 1

/** Profile màn — xem board_lcd_panel.h / menuconfig "Man hinh LCD". */
#if BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_GMT028_28
#define BOARD_LCD_GMT028 1
#define BOARD_LCD_MIRROR_X_AFTER_INIT 0
#define BOARD_LCD_MIRROR_Y_AFTER_INIT 0
#define BOARD_LCD_PANEL_INVERT 1
#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#elif BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_ILI9341_LEGACY_28
#define BOARD_LCD_GMT028 0
#define BOARD_LCD_MIRROR_X_AFTER_INIT 1
#define BOARD_LCD_MIRROR_Y_AFTER_INIT 1
#define BOARD_LCD_PANEL_INVERT 0
#define BOARD_LCD_PCLK_HZ (26 * 1000 * 1000)
#else
#error "BOARD_LCD_PANEL_ID: chi ho tro 1 (GMT028) hoac 2 (legacy 2.8)"
#endif
#define BOARD_LCD_SPI_MODE 0
/** 24 dòng/chunk: ít lần ghi SPI hơn → ít răng cưa khi flush LVGL trên 240px. */
#define BOARD_LCD_SPI_CHUNK_LINES 24
#define BOARD_LCD_POST_DISPON_DELAY_MS 80

/**
 * Nền UI (`lcd_ui` UI_BG), xóa màn sau init, viền letterbox JPEG — MỘT nguồn RGB888.
 * Viền KHÔNG được qua BOARD_SD_IMAGE_SWAP_RB (swap chỉ sửa thứ tự kênh *file ảnh*, ám viền nâu nếu áp nhầm).
 * Nếu vẫn ám nâu: thử R=G=B (vd. 248,248,248) thay ba số dưới.
 */
#ifndef BOARD_UI_BG_R
#define BOARD_UI_BG_R 235
#endif
#ifndef BOARD_UI_BG_G
#define BOARD_UI_BG_G 238
#endif
#ifndef BOARD_UI_BG_B
#define BOARD_UI_BG_B 242
#endif

/**
 * 0: nen toi sau init (san xuat).
 * 1: nen do thuan sau init — debug: neu thay do = RAMWR OK; neu thay mau khac = sai BGR/invert.
 * Dat lai ve 0 sau khi debug xong mau.
 */
#ifndef BOARD_LCD_STARTUP_SOLID_RED_TEST
#define BOARD_LCD_STARTUP_SOLID_RED_TEST 0
#endif
#if BOARD_LCD_STARTUP_SOLID_RED_TEST
#ifndef BOARD_LCD_STARTUP_CLEAR_R
#define BOARD_LCD_STARTUP_CLEAR_R 255
#endif
#ifndef BOARD_LCD_STARTUP_CLEAR_G
#define BOARD_LCD_STARTUP_CLEAR_G 0
#endif
#ifndef BOARD_LCD_STARTUP_CLEAR_B
#define BOARD_LCD_STARTUP_CLEAR_B 0
#endif
#else
/* Nền sáng #F1F5F9 — khớp màn chờ LVGL (không dùng #0F172A tối). */
#define BOARD_LCD_STARTUP_CLEAR_R 241
#define BOARD_LCD_STARTUP_CLEAR_G 245
#define BOARD_LCD_STARTUP_CLEAR_B 249
#endif

/**
 * Khi LCD và SD cùng SPI2, DMA khung hình RGB565 có thể > 8192 byte — tăng
 * max_transfer_sz trên bus (bên init trước quyết định; sd_card dùng giá trị này).
 */
#if BOARD_LCD_SHARE_SPI2_WITH_SD
#ifndef BOARD_SD_SPI_MAX_TRANSFER_SZ
#define BOARD_SD_SPI_MAX_TRANSFER_SZ \
    ((unsigned)(BOARD_LCD_H_RES) * (unsigned)(BOARD_LCD_V_RES) * 2u > 8192u \
         ? (unsigned)(BOARD_LCD_H_RES) * (unsigned)(BOARD_LCD_V_RES) * 2u \
         : 8192u)
#endif
#else
#ifndef BOARD_SD_SPI_MAX_TRANSFER_SZ
#define BOARD_SD_SPI_MAX_TRANSFER_SZ 8192
#endif
#endif

#ifndef BOARD_LCD_RGB565_SWAP_BYTES
#define BOARD_LCD_RGB565_SWAP_BYTES 0
#endif

/**
 * RGB888 -> RGB565 — dùng qua lcd_color.h / BOARD_RGB565_FROM888 (lcd_ui, sd_png).
 */
#ifndef BOARD_RGB565_FROM888
#define BOARD_RGB565_FROM888(r, g, b)                                                                                  \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | (((uint16_t)(b)) >> 3)))
#endif

/**
 * Anh JPEG (chi pixel anh, khong anh huong letterbox/UI): hoan R<->B truoc khi RGB565.
 * Da nguoi xanh troi / anh gan nhu den trang do lech kenh: thu 1 (mac dinh). Neu mau la hon: 0.
 */
#ifndef BOARD_SD_IMAGE_SWAP_RB_CHANNELS
#define BOARD_SD_IMAGE_SWAP_RB_CHANNELS 0
#endif

/**
 * Viền letterbox: mặc định = BOARD_UI_BG_* — cùng mã hóa với nền lcd_ui (lcd_color_from888 + to_bus).
 * Viền đồng bộ nền UI; không qua BOARD_SD_IMAGE_SWAP_RB (chỉ sửa kênh file JPEG).
 * Đổi riêng (vd. trắng 248,248,248) nếu cần; không qua BOARD_SD_IMAGE_SWAP_RB (chỉ sửa kênh file JPEG).
 */
#ifndef BOARD_SD_IMAGE_LETTERBOX_R
#define BOARD_SD_IMAGE_LETTERBOX_R BOARD_UI_BG_R
#endif
#ifndef BOARD_SD_IMAGE_LETTERBOX_G
#define BOARD_SD_IMAGE_LETTERBOX_G BOARD_UI_BG_G
#endif
#ifndef BOARD_SD_IMAGE_LETTERBOX_B
#define BOARD_SD_IMAGE_LETTERBOX_B BOARD_UI_BG_B
#endif

/**
 * 1: đảo thứ tự cột trong mỗi glyph 8x8 (bit0 ↔ bit7). Dùng nếu vẫn thấy từng
 * chữ bị lật nhưng không muốn đổi MIRROR_X (vd. ảnh JPEG đã đúng, chỉ font
 * sai).
 */
#ifndef BOARD_LCD_FONT_FLIP_H
#define BOARD_LCD_FONT_FLIP_H 0
#endif

/**
 * 1: vẽ chuỗi theo thứ tự ngược (ký tự cuối bên trái) — khi MADCTL/mirror làm
 * trục X ngược với thứ tự đọc mong muốn. 0: giữ thứ tự ký tự như trong code.
 */
#ifndef BOARD_LCD_TEXT_REVERSE_ORDER
#define BOARD_LCD_TEXT_REVERSE_ORDER 0
#endif

#if BOARD_RC522_SHARE_SD_SPI_BUS
/** RC522 — chung SPI2 với SD (SCK/MOSI/MISO = 35/36/37); CS RC522=4, CS thẻ=5. */
#define RC522_SCK_GPIO BOARD_SD_SCK_GPIO
#define RC522_MOSI_GPIO BOARD_SD_MOSI_GPIO
#define RC522_MISO_GPIO BOARD_SD_MISO_GPIO
#define RC522_CS_GPIO GPIO_NUM_4
#else
/** RC522 — bit-bang, bus SPI riêng với SD. */
#define RC522_SCK_GPIO GPIO_NUM_11
#define RC522_MOSI_GPIO GPIO_NUM_12
#define RC522_MISO_GPIO GPIO_NUM_13
#define RC522_CS_GPIO GPIO_NUM_4
#endif
#define RC522_RST_GPIO GPIO_NUM_3
/** GPIO_NUM_NC = không nối / không dùng chân IRQ trên code. */
#define RC522_IRQ_GPIO GPIO_NUM_NC

/**
 * MAX98357A (I2S TX) — không trùng SPI LCD / SD / RC522.
 * BCLK=16, LRC(WS)=17, DIN ampli = dout ESP GPIO18.
 * Loa: 8Ω 1,5W — gain PCM mặc định 2/5 (~40%). To hơn: 4/5; méo: giảm DEN.
 */
#ifndef BOARD_SPEAKER_OHM
#define BOARD_SPEAKER_OHM 8
#endif
#ifndef BOARD_SPEAKER_POWER_W_x10
#define BOARD_SPEAKER_POWER_W_x10 15 /* 1,5 W */
#endif
/** Biên độ PCM sau decode WAV: sample_out = sample * NUM / DEN (trước clip int16). */
#ifndef BOARD_AUDIO_PCM_GAIN_NUM
#define BOARD_AUDIO_PCM_GAIN_NUM 5
#endif
#ifndef BOARD_AUDIO_PCM_GAIN_DEN
#define BOARD_AUDIO_PCM_GAIN_DEN 5
#endif
#ifndef BOARD_I2S_BCLK_GPIO
#define BOARD_I2S_BCLK_GPIO GPIO_NUM_16
#endif
#ifndef BOARD_I2S_WS_GPIO
#define BOARD_I2S_WS_GPIO GPIO_NUM_17
#endif
#ifndef BOARD_I2S_DOUT_GPIO
#define BOARD_I2S_DOUT_GPIO GPIO_NUM_18
#endif
/**
 * 0 = tắt I2S/loa (không task WAV). 1 = bật.
 * Tắt thì ảnh từ SD lên màn thường mượt hơn: bớt tranh CPU/DMA + đọc thẻ (decode JPEG/PNG tốn PSRAM/CPU).
 */
#ifndef BOARD_ENABLE_AUDIO
#define BOARD_ENABLE_AUDIO 1
#endif
#if BOARD_SD_ONLY || BOARD_RFID_ONLY
#undef BOARD_ENABLE_AUDIO
#define BOARD_ENABLE_AUDIO 0
#endif
/**
 * 1: boot xếp hàng phát 1.wav → 2.wav → 3.wav; bỏ app_audio_pause/resume khi ghi SD,
 * bỏ trễ BOARD_AUDIO_MS_*, bỏ phát 1.wav trên màn idle (trùng bài test). Chỉ dùng thử nghiệm.
 */
#ifndef BOARD_AUDIO_STRESS_TEST
/** 0=chạy bình thường. 1=chỉ thử nghiệm (1+2+3 boot, bỏ pause) — dễ lỗi bus/timing; để 0 nếu ổn định. */
#define BOARD_AUDIO_STRESS_TEST 0
#endif
/** Khi bật audio: ưu tiên thấp hơn màn/ảnh/RFID (FreeRTOS, số càng lớn càng ưu). Mặc định 3. */
#ifndef BOARD_AUDIO_TASK_PRIO
#define BOARD_AUDIO_TASK_PRIO 7
#endif
/**
 * Số ms nghỉ trước khi xếp hàng 2/3.wav sau khi vẽ ảnh (decode + SPI) — 0 = không trễ.
 * ~40–100 ms thường giúm mượt màn hơn nếu vừa phát vừa hiện ảnh.
 */
#ifndef BOARD_AUDIO_MS_AFTER_IMAGE
#define BOARD_AUDIO_MS_AFTER_IMAGE 100
#endif
/**
 * Trước 1.wav phát sau cùng idle screen (ms) — 0 = tắt. Giảm xung đột vẽ 1s + đọc SD cùng lúc.
 */
#ifndef BOARD_AUDIO_MS_AFTER_IDLE_PAINT
#define BOARD_AUDIO_MS_AFTER_IDLE_PAINT 80
#endif
