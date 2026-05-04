#pragma once

/**
 * =============================================================================
 * CẤU HÌNH PHẦN CỨNG — CHỈ SỬA GPIO TẠI ĐÂY CHO KHỚP MẠCH ĐÃ HÀN.
 * SD = SPI3 (CS=5…); LCD 3.5" SPI2 — ILI9488 hoặc ILI9486 (`BOARD_LCD_PANEL_PROFILE`).
 * =============================================================================
 */

#include "driver/gpio.h"
#include "hal/spi_types.h"

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
 *   - LCD → SPI2 (39/40/41…); SD → SPI3 (21/47/48, CS 5); RC522 → bit-bang 11/12/13, CS 4, RST 3.
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
 */
#ifndef BOARD_SD_SPI_MAX_FREQ_KHZ
#define BOARD_SD_SPI_MAX_FREQ_KHZ 10000
#endif
/** Chờ sau khi CS cao + bus init trước mount (ms); thẻ/module chậm nguồn có thể cần 100–200. */
#ifndef BOARD_SD_PRE_MOUNT_DELAY_MS
#define BOARD_SD_PRE_MOUNT_DELAY_MS 100
#endif

/**
 * Profile khởi tạo LCD (chip trên màn RPi 3.5" hay nhầm nhãn):
 *   0 = ILI9488 (TFT_eSPI ILI9488_Init.h — Bodmer)
 *   1 = ILI9486 SPI (TFT_eSPI ILI9486_Init.h — fb_ili9486 / nhiều màn “9488” thực tế là 9486)
 */
#ifndef BOARD_LCD_PANEL_PROFILE
#define BOARD_LCD_PANEL_PROFILE 1
#endif

/**
 * 1: đọc RDDID sau init — chỉ có ý nghĩa nếu SDO của chip TFT thật sự nối tới chân MISO bạn dùng.
 * Trên nhiều shield Pi 3.5", header pin 21 (MISO) chỉ nối touch XPT2046, không nối SDO ILI9488 → luôn FF.
 */
#ifndef BOARD_LCD_TRY_READ_LCD_ID
#define BOARD_LCD_TRY_READ_LCD_ID 1
#endif

#define BOARD_LCD_SPI_HOST SPI2_HOST
/** ESP32-S3: OK. ESP32 cổ điển: GPIO34–39 chỉ INPUT — không thể làm SCK/MOSI. */
#define BOARD_LCD_SCK_GPIO GPIO_NUM_39
#define BOARD_LCD_MOSI_GPIO GPIO_NUM_40
/** Pi pin 21 thường gọi MISO — kiểm tra schematic: có thể là MISO touch, không phải TFT SDO. */
#define BOARD_LCD_MISO_GPIO GPIO_NUM_41
#define BOARD_LCD_DC_GPIO GPIO_NUM_9
#define BOARD_LCD_CS_GPIO GPIO_NUM_10
#define BOARD_LCD_RST_GPIO GPIO_NUM_8
/** 1: LCD và SD cùng SPI2/3 — lcd_ui chấp nhận bus đã init (ESP_ERR_INVALID_STATE). */
#ifndef BOARD_LCD_SHARE_SPI2_WITH_SD
#define BOARD_LCD_SHARE_SPI2_WITH_SD 0
#endif

/**
 * Touch resistive (XPT2046 hay tương đương trên bo màn): PIN26→TP_CS→GPIO7, PIN11→TP_IRQ→GPIO6.
 * Chưa có driver trong repo — chỉ khai chân để nối sau.
 */
#ifndef BOARD_TOUCH_CS_GPIO
#define BOARD_TOUCH_CS_GPIO GPIO_NUM_7
#endif
#ifndef BOARD_TOUCH_IRQ_GPIO
#define BOARD_TOUCH_IRQ_GPIO GPIO_NUM_6
#endif

/**
 * Đèn nền LCD (nếu màn có chân BL riêng). GPIO_NUM_NC = không điều khiển từ
 * code (BL nối VCC trên mạch).
 */
#ifndef BOARD_LCD_BL_GPIO
#define BOARD_LCD_BL_GPIO GPIO_NUM_NC
#endif
#ifndef BOARD_LCD_BL_ACTIVE_HIGH
#define BOARD_LCD_BL_ACTIVE_HIGH 1
#endif

/** Bit MX (cột); +BGR trong driver → 0x48 như TFT_eSPI ILI9488_Init.h */
#ifndef BOARD_ILI9488_MADCTL_BASE
#define BOARD_ILI9488_MADCTL_BASE 0x40
#endif
#ifndef BOARD_ILI9488_COLMOD
#define BOARD_ILI9488_COLMOD 0x55
#endif

/**
 * SPI ILI9488: nhiều module chỉ nhận pixel 18-bit (0x3A=0x66), không hiển thị đúng với 0x55.
 * 1: COLMOD 0x66 + chuyển RGB565→RGB666 trong driver (esp_lcd_ili9488 / lovyan).
 * 0: gửi thẳng RGB565 (ít gặp trên SPI).
 */
/** Màn RPi SPI nhiều lô chạy tốt với 0x55 (RGB565). Chỉ bật 1 nếu 0x55 vẫn sai màu/trắng. */
#ifndef BOARD_LCD_ILI9488_SPI_RGB666
#define BOARD_LCD_ILI9488_SPI_RGB666 1
#endif

/**
 * ILI9488 0xB0 (Interface Mode). Trắng toàn màn: thử 0 rồi 0x80.
 * Đọc RDDID/MISO toàn FF sau init: thử 0x00 — bit 0x80 trên vài bo làm SDO không dùng cho đọc.
 */
#ifndef BOARD_LCD_ILI9488_B0
#define BOARD_LCD_ILI9488_B0 0x00
#endif

/**
 * 1: khi MADCTL có MV (swap_xy), đổi nội dung CASET↔RASET — nhiều màn landscape cần.
 * 0: luôn CASET=X, RASET=Y (thử nếu hình vẫn sai sau bản có swap).
 */
/** 0 = CASET=X RASET=Y (thử trước nếu màn trắng + MV). 1 = đổi khi swap_xy. */
#ifndef BOARD_LCD_ILI9488_SWAP_ADDR_WINDOW_WITH_MV
#define BOARD_LCD_ILI9488_SWAP_ADDR_WINDOW_WITH_MV 0
#endif

/** 1: gắn MISO LCD vào bus SPI2 (đọc ID / debug); 0 nếu chân không nối. */
#ifndef BOARD_LCD_SPI_USE_MISO
#define BOARD_LCD_SPI_USE_MISO 1
#endif

/** 1: trước DISPON gửi 0x38 IDMOFF (chuỗi Lovyan/atanisoft). */
#ifndef BOARD_LCD_IDMOFF_BEFORE_DISPON
#define BOARD_LCD_IDMOFF_BEFORE_DISPON 1
#endif

#define BOARD_LCD_H_RES 320
#define BOARD_LCD_V_RES 480

/** 1: INVON (0x21). Chuỗi Bodmer đã set B4/B6 — thử 1 nếu màn ngả màu / IPS. */
#ifndef BOARD_LCD_PANEL_INVERT
#define BOARD_LCD_PANEL_INVERT 1
#endif

/**
 * 1: nền đỏ thuần sau init — nếu vẫn trắng = RAMWR/init sai; nếu thấy đỏ = căn màu/hướng.
 * 0: nền xanh đậm (sản xuất).
 */
#ifndef BOARD_LCD_STARTUP_SOLID_RED_TEST
#define BOARD_LCD_STARTUP_SOLID_RED_TEST 1
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
#ifndef BOARD_LCD_STARTUP_CLEAR_R
#define BOARD_LCD_STARTUP_CLEAR_R 6
#endif
#ifndef BOARD_LCD_STARTUP_CLEAR_G
#define BOARD_LCD_STARTUP_CLEAR_G 10
#endif
#ifndef BOARD_LCD_STARTUP_CLEAR_B
#define BOARD_LCD_STARTUP_CLEAR_B 28
#endif
#endif

/**
 * Số dòng mỗi lần ghi SPI RGB565 (480 × N × 2 byte). Giảm (vd. 20) nếu log
 * panel_io_spi_tx_color / ramwr failed khi RAM căng — dễ malloc DMA hơn.
 */
#ifndef BOARD_LCD_SPI_CHUNK_LINES
#define BOARD_LCD_SPI_CHUNK_LINES 24
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

/**
 * Byte swap for RGB565 (standard for most SPI LCDs).
 */
#ifndef BOARD_LCD_RGB565_SWAP_BYTES
#define BOARD_LCD_RGB565_SWAP_BYTES 0
#endif

/**
 * 1: BGR — IPS panels usually use BGR order.
 */
#ifndef BOARD_LCD_USE_BGR_ELEMENT_ORDER
#define BOARD_LCD_USE_BGR_ELEMENT_ORDER 1
#endif


/**
 * Sau init: MADCTL swap_xy (= MV). Thứ tự trong lcd_ui: swap_xy trước, mirror sau.
 * Waveshare 3.5" Pi (480×320 vật lý): logic 320×480 — swap=1 để hình phủ hết mặt,
 * tránh chỉ một cụm màu trong khung nhỏ + viền đen.
 * Nếu màn của bạn đã full sẵn: #define BOARD_LCD_SWAP_XY_AFTER_INIT 0 trước include board_pins.h.
 */
#ifndef BOARD_LCD_SWAP_XY_AFTER_INIT
#define BOARD_LCD_SWAP_XY_AFTER_INIT 0
#endif
/** Viền đen + cụm hình nhỏ: thử tắt gương (0,0) trước; bật từng bit nếu ngược trục. */
#ifndef BOARD_LCD_MIRROR_X_AFTER_INIT
#define BOARD_LCD_MIRROR_X_AFTER_INIT 0
#endif
#ifndef BOARD_LCD_MIRROR_Y_AFTER_INIT
#define BOARD_LCD_MIRROR_Y_AFTER_INIT 0
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

/** ms chờ sau DISPON trước khi RAMWR xóa nền (ổn định nguồn / gate) */
#ifndef BOARD_LCD_POST_DISPON_DELAY_MS
#define BOARD_LCD_POST_DISPON_DELAY_MS 120
#endif

/**
 * Xung SPI màn hình. 2 MHz đôi khi gây nhiễu/chấm mép màn (dây dài,
 * breadboard). Thử 1 MHz hoặc 500 kHz nếu thấy sọc / điểm lạ một bên.
 */
#ifndef BOARD_LCD_PCLK_HZ
#define BOARD_LCD_PCLK_HZ (20 * 1000 * 1000)
#endif
/** SPI mode 0 hầu hết ILI9488; thử 3 nếu nối dây dài / nhiễu. */
#ifndef BOARD_LCD_SPI_MODE
#define BOARD_LCD_SPI_MODE 0
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
 */
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
#define BOARD_AUDIO_TASK_PRIO 3
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
