# ESP32-S3 RFID Attendance

Firmware chấm công RFID trên **ESP32-S3** (ESP-IDF v5.x): RC522, LCD ILI9341/LVGL, WiFi portal, Azure IoT Hub MQTT, SD card, âm thanh I2S.

**Repo:** [github.com/MebiEco/RFID](https://github.com/MebiEco/RFID) · **Firmware:** `2.5.1` (`PROJECT_VER` trong `CMakeLists.txt`)

| | |
|---|---|
| Chip | ESP32-S3 + PSRAM (khuyến nghị ≥ 2 MB) |
| Framework | [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/) v5.x |
| UI | LVGL 8.3 + ILI9341 320×240 |
| Cloud | Azure IoT Hub (MQTT + SAS) |

---

## Tính năng

- Đọc thẻ **RC522** (MIFARE), tra profile trên SD, ghi log CSV
- Màn hình LVGL tiếng Việt (font Arial), popup kết quả quẹt
- **WiFi Portal** (SoftAP `192.168.4.1` khi chưa STA; khi có LAN dùng IP STA)
- **Azure IoT Hub**: telemetry QoS1, queue SD khi offline, Direct Method
- SD FAT32: profile, ảnh, WAV, `rfid_log.csv`
- Loa **MAX98357A** (I2S) phát WAV vào/ra
- OTA qua URL hoặc upload `.bin` trên portal
- Tổng quan chấm công / nhật ký trên web (chunked HTTP, đọc từ EOF)

### Ưu tiên vận hành

| Ưu tiên | Thành phần | Ghi chú |
|--------:|------------|---------|
| 1 | Quẹt thẻ + gửi Azure | Luôn bật (mặc định) |
| 2 | Portal web | Nhường SD/RAM khi đang quẹt hoặc đẩy MQTT |
| 3 | OTA | Tạm dừng RFID/Azure/audio để ghi flash; fail thì khôi phục |

---

## Phần cứng (GPIO mặc định)

Định nghĩa đầy đủ trong [`main/board_pins.h`](main/board_pins.h).

### LCD — SPI2

| Tín hiệu | GPIO |
|----------|------|
| SCK | 39 |
| MOSI | 40 |
| MISO | 41 |
| DC | 9 |
| CS | 10 |
| RST | 8 |

### RC522 — bit-bang (mặc định tách bus SD)

| Tín hiệu | GPIO |
|----------|------|
| SCK | 11 |
| MOSI | 12 |
| MISO | 13 |
| CS | 4 |
| RST | 3 |

> Có thể gộp RC522 với SD (`BOARD_RC522_SHARE_SD_SPI_BUS 1`).

### SD Card — SPI3

| Tín hiệu | GPIO |
|----------|------|
| SCK | 21 |
| MOSI | 47 |
| MISO | 48 |
| CS | 5 |

### MAX98357A — I2S

| Tín hiệu | GPIO |
|----------|------|
| BCLK | 16 |
| WS | 17 |
| DIN | 18 |

---

## Build & flash

**Yêu cầu:** ESP-IDF v5.x, Python 3.8+, CMake ≥ 3.16.

```bash
git clone https://github.com/MebiEco/RFID.git
cd RFID
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Chọn profile LCD trong `menuconfig` → **Man hinh LCD**:

- `1` — GMT028 2.8″ (khuyến nghị)
- `2` — ILI9341 Legacy 2.8″

Sau khi sửa `main/portal.html`, đóng gói lại gzip:

```bash
python tools/pack_portal.py
```

Thay đổi `sdkconfig.defaults` (ví dụ `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`) cần **xóa thư mục `build` rồi build lại** thì mới có hiệu lực đầy đủ.

---

## Cấu trúc repo

```
RFID/
├── main/
│   ├── main.c                 # Khởi tạo hệ thống
│   ├── board_pins.h           # GPIO & feature flags
│   ├── app_rfid.c/h           # Task quẹt thẻ
│   ├── mfrc522.c/h            # Driver RC522
│   ├── card_profile.c/h       # Hồ sơ thẻ trên SD
│   ├── attendance_day.c/h     # API tổng quan chấm công
│   ├── scan_log.c/h           # Log CSV + API nhật ký
│   ├── wifi_portal.c/h        # SoftAP / STA + httpd
│   ├── portal_web.c/h         # API portal + HTML gzip
│   ├── portal.html[.gz]       # UI quản trị
│   ├── app_azure.c/h          # MQTT Azure IoT Hub
│   ├── app_audio.c/h          # Phát WAV I2S
│   ├── app_ota.c/h            # OTA
│   ├── lcd_ui.c / lv_port*.c  # LCD + LVGL
│   ├── sd_card.c/h            # Mount SDSPI + DMA bounce
│   └── ds3231.c/h             # RTC (tuỳ chọn)
├── tools/pack_portal.py
├── azure_commands.txt         # Direct Method & telemetry (chi tiết)
├── partitions.csv
├── sdkconfig.defaults
└── CMakeLists.txt
```

---

## Feature flags (`board_pins.h`)

```c
#define BOARD_ENABLE_LCD    1
#define BOARD_ENABLE_RFID   1
#define BOARD_ENABLE_SD     1
#define BOARD_ENABLE_WIFI   1
#define BOARD_ENABLE_AZURE  1
#define BOARD_ENABLE_AUDIO  1

#define BOARD_SD_ONLY       0   /* chỉ test SD */
#define BOARD_RFID_ONLY     0   /* chỉ test RC522 */

#define BOARD_LOCAL_UTC_OFFSET_SEC  (7 * 3600)
```

---

## SD card

```
/sdcard/
├── profiles/
│   ├── <UID>.txt          # Tên | Mã NV
│   ├── <UID>.jpg          # Ảnh (tuỳ chọn)
│   └── default.jpg
├── checkin/               # Trạng thái vào/ra theo UID
├── audio/
│   ├── 1.wav … 4.wav
└── rfid_log.csv           # Log chấm công
```

---

## Web portal

1. Chưa có WiFi STA: kết nối SoftAP thiết bị → `http://192.168.4.1`
2. Đã nối LAN: mở IP mà thiết bị nhận từ DHCP

Đăng nhập admin (PIN/tài khoản trên portal). Các mục chính: Tổng quan, WiFi, Azure, Nhật ký, Thẻ, Cấu hình LCD, Hardware, Terminal, OTA.

Đổi mục menu sẽ **abort request cũ** để nhả socket/Internal trên thiết bị.

---

## Azure IoT Hub

Cấu hình **HostName**, **Device ID**, **SAS key** trên portal (lưu NVS). Chi tiết Direct Method: [`azure_commands.txt`](azure_commands.txt).

### Telemetry (MQTT)

```json
{
  "Code": 602,
  "Index": 3919,
  "TimeStamp": 1787650593,
  "Data": {
    "DeviceName": "RFID_Scanner",
    "UID": "544C38C7",
    "Name": "Nguyen Van A",
    "ID": "NV0003"
  }
}
```

| Code | Ý nghĩa |
|------|---------|
| 601 | Thẻ chưa đăng ký |
| 602 | Quẹt thẻ đã đăng ký |
| 603 | Lưu / sửa thẻ |
| 604 | Xóa thẻ |
| 605 | Flush / đồng bộ queue |
| 606 | Trigger OTA |
| 607 | Đổi Hub (ChangeHub) |

Offline / chưa NTP: ghi hàng đợi SD, flush khi MQTT sẵn sàng.

---

## Dependencies

| Thành phần | Phiên bản | Nguồn |
|------------|-----------|--------|
| ESP-IDF | v5.x | Espressif |
| LVGL | ^8.3.0 | IDF Component Manager |
| esp_lcd_ili9341 | ^2.0.0 | IDF Component Manager |

---

## Troubleshooting

**RC522 không đọc thẻ** — kiểm tra 3.3V và GPIO; thử `RC522_SPI_CLOCK_HZ (500000)`.

**LCD sai màu / lật** — chọn đúng profile trong menuconfig; debug `BOARD_LCD_STARTUP_SOLID_RED_TEST 1`.

**SD mount lỗi** — nguồn yếu khi WiFi bật; giảm `BOARD_SD_SPI_MAX_FREQ_KHZ`; tăng `BOARD_SD_PRE_MOUNT_DELAY_MS`.

**Azure không MQTT** — cần WiFi + NTP (năm ≥ 2020); kiểm tra HostName `*.azure-devices.net` và hạn SAS.

**Internal / DMA thấp khi mở web** — đóng tab Nhật ký/Tổng quan; đo Hardware khi **không** bật Auto poll. An toàn tham khảo: Internal trống ≥ 20 KB, DMA block ≥ 8 KB.

**OTA** — tạm dừng quẹt/Azure là bình thường; OTA fail sẽ khôi phục dịch vụ.

---

## Đóng góp

1. Fork → branch `feature/...`
2. Commit rõ mục đích
3. Mở Pull Request trên [MebiEco/RFID](https://github.com/MebiEco/RFID)

---

## License

Phần mềm dùng cho dự án MebiEco. Nếu cần giấy phép nguồn mở cụ thể (MIT/Apache-2.0), bổ sung file `LICENSE` trong repo.
