# 🪪 ESP32-S3 RFID Attendance Kiosk

<div align="center">

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red?style=for-the-badge&logo=espressif&logoColor=white)
![LVGL](https://img.shields.io/badge/LVGL-v8.3-blue?style=for-the-badge)
![Azure IoT](https://img.shields.io/badge/Azure%20IoT%20Hub-MQTT-0089D6?style=for-the-badge&logo=microsoftazure&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-green?style=for-the-badge)
![Platform](https://img.shields.io/badge/platform-ESP32--S3-orange?style=for-the-badge)

**Máy chấm công thẻ từ (RFID) thông minh chạy trên ESP32-S3**
*RC522 · Màn hình ILI9341 2.8" · WiFi Portal · Azure IoT Hub · SD Card · Âm thanh*

</div>

---

## ✨ Tính Năng Nổi Bật

| Tính năng | Mô tả |
|-----------|-------|
| 📡 **Đọc thẻ RFID** | RC522 (MFRC522) giao tiếp SPI/bit-bang, hỗ trợ thẻ Mifare |
| 🖥️ **Màn hình LVGL** | ILI9341 2.8″ 320×240, giao diện đồ họa tiếng Việt với font Arial |
| 🌐 **WiFi Portal** | SoftAP + Web UI tại `192.168.4.1` — cấu hình WiFi, Azure, thẻ, OTA |
| ☁️ **Azure IoT Hub** | Gửi telemetry qua MQTT + SAS Token, tự resend khi mất kết nối |
| 💾 **SD Card** | Lưu log CSV chấm công, profile ảnh nhân viên (JPEG), file WAV |
| 🔊 **Âm thanh** | MAX98357A (I2S), phát WAV chào vào/ra từ SD Card |
| 🔄 **OTA Update** | Cập nhật firmware qua mạng không cần cắm USB |
| 🔐 **Quản lý thẻ** | Thêm/sửa/xóa thẻ qua Web Portal hoặc màn hình |

---

## 🏗️ Kiến Trúc Hệ Thống

```
┌──────────────────────────────────────────────────────────┐
│                      ESP32-S3                            │
│                                                          │
│  ┌──────────┐   SPI2   ┌─────────────┐                  │
│  │  ILI9341 │◄────────►│  LCD Driver │◄── LVGL v8.3     │
│  │  2.8"    │          │  (esp_lcd)  │                  │
│  └──────────┘          └─────────────┘                  │
│                                                          │
│  ┌──────────┐  Bit-bang ┌─────────────┐                 │
│  │  RC522   │◄─────────►│  app_rfid   │◄── Card Profile │
│  │  MFRC522 │           └─────────────┘                 │
│  └──────────┘                                           │
│                                                          │
│  ┌──────────┐   SPI3   ┌─────────────┐                  │
│  │  SD Card │◄────────►│  scan_log   │◄── CSV / JPEG    │
│  │  FAT32   │          │  sd_png/wav │                  │
│  └──────────┘          └─────────────┘                  │
│                                                          │
│  ┌──────────┐   I2S    ┌─────────────┐                  │
│  │ MAX98357A│◄────────►│  app_audio  │◄── WAV Player    │
│  │  Amp     │          └─────────────┘                  │
│  └──────────┘                                           │
│                                                          │
│  ┌────────────────────┐  ┌──────────────────────┐       │
│  │   wifi_portal      │  │    app_azure          │       │
│  │  SoftAP + Web UI  │  │  MQTT + SAS Token    │       │
│  │  192.168.4.1      │  │  Azure IoT Hub       │       │
│  └────────────────────┘  └──────────────────────┘       │
└──────────────────────────────────────────────────────────┘
```

---

## 📦 Yêu Cầu Phần Cứng

### Bo mạch chính
- **ESP32-S3** (khuyến nghị module với PSRAM — ít nhất 2 MB PSRAM để decode JPEG/LVGL)

### Kết nối GPIO (mặc định)

#### 🟢 Màn hình LCD — SPI2
| Tín hiệu | GPIO |
|-----------|------|
| SCK       | 39   |
| MOSI      | 40   |
| MISO      | 41   |
| DC        | 9    |
| CS        | 10   |
| RST       | 8    |

#### 🔵 RC522 RFID — Bit-bang (SPI riêng)
| Tín hiệu | GPIO |
|-----------|------|
| SCK       | 11   |
| MOSI      | 12   |
| MISO      | 13   |
| CS        | 4    |
| RST       | 3    |

#### 🟡 SD Card — SPI3
| Tín hiệu | GPIO |
|-----------|------|
| SCK       | 21   |
| MOSI      | 47   |
| MISO      | 48   |
| CS        | 5    |

#### 🔴 Loa MAX98357A — I2S
| Tín hiệu | GPIO |
|-----------|------|
| BCLK      | 16   |
| WS (LRC)  | 17   |
| DIN       | 18   |

---

## 🛠️ Cài Đặt & Build

### Yêu cầu môi trường
- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- Python 3.8+
- CMake >= 3.16

### Clone và chuẩn bị

```bash
git clone https://github.com/<your-username>/RFID.git
cd RFID
```

### Cấu hình target

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

> **Lưu ý:** Vào `Component config → Man hinh LCD` để chọn đúng profile màn hình:
> - `1` → GMT028 2.8" (khuyến nghị)
> - `2` → ILI9341 Legacy 2.8"

### Build & Flash

```bash
idf.py build
idf.py -p COMx flash monitor
```

---

## 📂 Cấu Trúc Thư Mục

```
RFID/
├── main/
│   ├── main.c                  # Entry point, khởi tạo tất cả module
│   ├── board_pins.h            # ⚙️  Toàn bộ cấu hình GPIO & feature flags
│   ├── board_lcd_panel.h       # Profile màn hình LCD
│   │
│   ├── app_rfid.c/h            # Task quẹt thẻ + điều hướng UI
│   ├── mfrc522.c/h             # Driver RC522 bit-bang
│   ├── card_profile.c/h        # Lưu/đọc profile thẻ từ SD
│   │
│   ├── lcd_ui.c/h              # Vẽ UI gốc (SPI direct)
│   ├── lv_port.c/h             # LVGL port cho ILI9341
│   ├── lv_port_screens.c       # Các màn hình LVGL
│   ├── lcd_panel_config.c/h    # Init panel esp_lcd
│   │
│   ├── wifi_portal.c/h         # SoftAP + HTTP Web Server
│   ├── portal_web.c/h          # HTML/CSS/JS nhúng (captive portal)
│   │
│   ├── app_azure.c/h           # Azure IoT Hub MQTT client
│   ├── app_audio.c/h           # WAV player (I2S + MAX98357A)
│   ├── app_ota.c/h             # OTA firmware update
│   │
│   ├── scan_log.c/h            # Ghi/đọc log CSV chấm công
│   ├── sd_card.c/h             # Mount/unmount SD (SPI FAT32)
│   ├── sd_png.c/h              # Decode PNG từ SD
│   ├── sd_wav.c/h              # Đọc WAV từ SD
│   │
│   └── arial_vn_*.c            # Font tiếng Việt (24/32/48px)
│
├── CMakeLists.txt
├── partitions.csv              # Phân vùng flash tùy chỉnh
├── sdkconfig.defaults          # Cấu hình IDF mặc định
└── README.md
```

---

## ⚙️ Cấu Hình Nhanh (`board_pins.h`)

Tất cả tùy chọn phần cứng được điều khiển bằng `#define` trong `main/board_pins.h`:

```c
// Bật/tắt từng module
#define BOARD_ENABLE_LCD    1   // Màn hình ILI9341
#define BOARD_ENABLE_RFID   1   // Đầu đọc RC522
#define BOARD_ENABLE_SD     1   // Thẻ nhớ SD
#define BOARD_ENABLE_WIFI   1   // WiFi + Web Portal
#define BOARD_ENABLE_AZURE  1   // Azure IoT Hub
#define BOARD_ENABLE_AUDIO  1   // Loa MAX98357A

// Chế độ debug nhanh
#define BOARD_SD_ONLY    0      // Chỉ test SD Card
#define BOARD_RFID_ONLY  0      // Chỉ test RC522

// Múi giờ (giây UTC offset, Việt Nam = +7h)
#define BOARD_LOCAL_UTC_OFFSET_SEC  (7 * 3600)
```

---

## 🗂️ Cấu Trúc SD Card

```
/sdcard/
├── profiles/
│   ├── <UID>.txt           # Hồ sơ thẻ: Tên | Mã nhân viên
│   ├── <UID>.jpg           # Ảnh nhân viên (JPEG)
│   └── default.jpg         # Ảnh mặc định khi không có ảnh riêng
├── audio/
│   ├── 1.wav               # Phát khi kết nối WiFi thành công
│   ├── 2.wav               # Phát khi chấm công vào (IN)
│   ├── 3.wav               # Phát khi chấm công ra (OUT)
│   └── 4.wav               # Xác nhận lưu/xóa thành công
└── rfid_log.csv            # Log chấm công (UID, Tên, Mã, Timestamp)
```

---

## 🌐 Web Portal

Khi thiết bị khởi động, nó tạo một **WiFi Access Point**:
- **SSID:** `ESP32-RFID` (hoặc tên cấu hình)
- **Địa chỉ:** `http://192.168.4.1`

### Các trang quản lý

| Đường dẫn | Chức năng |
|-----------|-----------|
| `/`        | Trang chủ, trạng thái hệ thống |
| `/wifi`    | Quản lý kết nối WiFi |
| `/cards`   | Thêm / Sửa / Xóa thẻ |
| `/scans`   | Xem lịch sử chấm công |
| `/azure`   | Cấu hình Azure IoT Hub |
| `/ota`     | Cập nhật firmware OTA |

---

## ☁️ Azure IoT Hub

Thiết bị gửi **telemetry JSON** lên Azure IoT Hub qua MQTT:

```json
{
  "uid": "A1B2C3D4",
  "name": "Nguyen Van A",
  "id": "NV001",
  "is_registered": 1,
  "event_code": 602,
  "msg_idx": 42,
  "timestamp": 1750000000
}
```

| `event_code` | Ý nghĩa |
|-------------|---------|
| `601` | Thẻ lạ chưa đăng ký |
| `602` | Quẹt thẻ hợp lệ |
| `603` | Lưu / Sửa thẻ |
| `604` | Xóa thẻ |

> Cấu hình **Hostname**, **Device ID** và **SAS Token** qua Web Portal (`/azure`).
> Token được lưu trong NVS — không mất khi reset.

---

## 🔧 Troubleshooting

<details>
<summary><b>RC522 không đọc được thẻ</b></summary>

- Kiểm tra kết nối GPIO `SCK=11 / MOSI=12 / MISO=13 / CS=4 / RST=3`
- Đảm bảo cấp đủ 3.3V cho module RC522
- Thử giảm tốc độ SPI: `RC522_SPI_CLOCK_HZ (500000)` trong `board_pins.h`
- Xem log: `RC522 bit-bang init OK (3-bus mode)` là init thành công

</details>

<details>
<summary><b>Màn hình hiển thị sai màu / bị lật</b></summary>

- Vào `menuconfig → Man hinh LCD` → chọn đúng profile (GMT028 hoặc Legacy)
- Debug màu: bật `BOARD_LCD_STARTUP_SOLID_RED_TEST 1` — nếu thấy đỏ là pipeline đúng
- Màn bị lật: thử `BOARD_LCD_MIRROR_X_AFTER_INIT / MIRROR_Y_AFTER_INIT`

</details>

<details>
<summary><b>SD Card mount thất bại</b></summary>

- Kiểm tra nguồn cấp: WiFi đang bật có thể gây sụt nguồn → dùng tụ lọc 100µF gần module SD
- Giảm tốc độ SPI SD: `BOARD_SD_SPI_MAX_FREQ_KHZ 1000`
- Tăng thời gian chờ trước mount: `BOARD_SD_PRE_MOUNT_DELAY_MS 1000`

</details>

<details>
<summary><b>Azure MQTT không kết nối</b></summary>

- Đảm bảo WiFi đã kết nối và SNTP đã đồng bộ (thời gian hợp lệ năm >= 2020)
- SAS Token có hạn sử dụng — tạo lại token mới trên Azure Portal
- Kiểm tra Hostname: `<hub-name>.azure-devices.net`

</details>

---

## 📋 Dependencies

| Thư viện | Phiên bản | Nguồn |
|----------|-----------|-------|
| ESP-IDF  | v5.x      | Espressif |
| LVGL     | ^8.3.0    | IDF Component Manager |
| esp_lcd_ili9341 | ^2.0.0 | IDF Component Manager |

---

## 🤝 Đóng Góp

Pull requests và issues đều được chào đón!

1. Fork repository này
2. Tạo branch mới: `git checkout -b feature/ten-tinh-nang`
3. Commit thay đổi: `git commit -m "feat: thêm tính năng XYZ"`
4. Push và tạo Pull Request

---

## 📄 License

Dự án này được phát hành dưới giấy phép **MIT**. Xem file [LICENSE](LICENSE) để biết thêm chi tiết.

---

<div align="center">

Made with ❤️ for Vietnamese IoT community

**ESP32-S3 · RFID · Azure IoT · LVGL · FreeRTOS**

</div>
