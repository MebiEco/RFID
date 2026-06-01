/* MFRC522 SPI — logic tương thích Arduino MFRC522 (địa chỉ register << 1). */
#include "mfrc522.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG_MF = "mfrc522";

/** RC522 bit-bang: ~250 kHz @ 2µs nửa chu kỳ — an toàn dây dài hơn 1µs. */
#ifndef MFRC522_BB_HALF_PERIOD_US
#define MFRC522_BB_HALF_PERIOD_US 1
#endif

static bool s_bb_mode;
static spi_device_handle_t s_hw_spi;
static gpio_num_t s_bb_sck;
static gpio_num_t s_bb_mosi;
static gpio_num_t s_bb_miso;
static gpio_num_t s_bb_cs;

enum {
    REG_CommandReg = 0x01 << 1,
    REG_ComIrqReg = 0x04 << 1,
    REG_DivIrqReg = 0x05 << 1,
    REG_ErrorReg = 0x06 << 1,
    REG_Status2Reg = 0x08 << 1,
    REG_FIFODataReg = 0x09 << 1,
    REG_FIFOLevelReg = 0x0A << 1,
    REG_ControlReg = 0x0C << 1,
    REG_BitFramingReg = 0x0D << 1,
    REG_CollReg = 0x0E << 1,
    REG_ModeReg = 0x11 << 1,
    REG_TxModeReg = 0x12 << 1,
    REG_RxModeReg = 0x13 << 1,
    REG_TxControlReg = 0x14 << 1,
    REG_TxASKReg = 0x15 << 1,
    REG_CRCResultRegH = 0x21 << 1,
    REG_CRCResultRegL = 0x22 << 1,
    REG_ModWidthReg = 0x24 << 1,
    REG_TModeReg = 0x2A << 1,
    REG_TPrescalerReg = 0x2B << 1,
    REG_TReloadRegH = 0x2C << 1,
    REG_TReloadRegL = 0x2D << 1,
    REG_VersionReg = 0x37 << 1,
    /** RFCfg — chỉnh RxGain (sóng thu 13,56 MHz, giúp REQA/ATQA nếu anten yếu) */
    REG_RFCfgReg = 0x26 << 1,
};

enum {
    PCD_Idle = 0x00,
    PCD_CalcCRC = 0x03,
    PCD_Transceive = 0x0C,
    PCD_SoftReset = 0x0F,
};

enum {
    PICC_CMD_REQA = 0x26,
    PICC_CMD_WUPA = 0x52,
    PICC_CMD_CT = 0x88,
    PICC_CMD_SEL_CL1 = 0x93
};

static uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/** Giao dịch SPI dài nhất (FIFO+1); không dùng chung tx/rx pointer — DMA ESP-IDF lỗi nếu trùng. */
#define MFRC522_SPI_XFER_MAX 66
static uint8_t s_spi_txb[MFRC522_SPI_XFER_MAX + 8] __attribute__((aligned(4)));
static uint8_t s_spi_rxb[MFRC522_SPI_XFER_MAX + 8] __attribute__((aligned(4)));
/** Một byte / lần giao dịch — con trỏ tx/rx thẳng hàng 4 (DMA an toàn hơn &s_spi_txb[i] khi i>0). */
static uint8_t s_spi_one_tx __attribute__((aligned(4)));
static uint8_t s_spi_one_rx __attribute__((aligned(4)));

static esp_err_t spi_tx_rx(spi_device_handle_t spi, uint8_t *buf, size_t len)
{
    if (s_bb_mode) {
        (void)spi;
        gpio_set_level(s_bb_cs, 0);
        esp_rom_delay_us(MFRC522_BB_HALF_PERIOD_US);
        for (size_t i = 0; i < len; i++) {
            uint8_t tx = buf[i];
            uint8_t rx = 0;
            for (int bit = 7; bit >= 0; bit--) {
                gpio_set_level(s_bb_mosi, (tx >> (unsigned)bit) & 1);
                esp_rom_delay_us(MFRC522_BB_HALF_PERIOD_US);
                gpio_set_level(s_bb_sck, 1);
                if (gpio_get_level(s_bb_miso)) {
                    rx |= (uint8_t)(1U << (unsigned)bit);
                }
                esp_rom_delay_us(MFRC522_BB_HALF_PERIOD_US);
                gpio_set_level(s_bb_sck, 0);
            }
            buf[i] = rx;
        }
        esp_rom_delay_us(MFRC522_BB_HALF_PERIOD_US);
        gpio_set_level(s_bb_cs, 1);
        return ESP_OK;
    }
    if (len > MFRC522_SPI_XFER_MAX) {
        len = MFRC522_SPI_XFER_MAX;
    }
    memcpy(s_spi_txb, buf, len);
    memset(s_spi_rxb, 0, len);
    /**
     * MFRC522: mỗi byte là một “phase” với CS thấp suốt (giống 2x SPI.transfer Arduino).
     * Một giao dịch SPI dài (DMA) trên ESP32 dễ lệch MISO / byte hợp — log VersionReg=0xBA, RFCfg sai.
     */
    for (size_t i = 0; i < len; i++) {
        s_spi_one_tx = s_spi_txb[i];
        spi_transaction_t t = {
            .length = 8,
            .tx_buffer = &s_spi_one_tx,
            .rx_buffer = &s_spi_one_rx,
            .flags = (i < len - 1) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        esp_err_t err = spi_device_transmit(spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        s_spi_rxb[i] = s_spi_one_rx;
    }
    memcpy(buf, s_spi_rxb, len);
    return ESP_OK;
}

static void pcd_write_reg(spi_device_handle_t spi, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    (void)spi_tx_rx(spi, buf, sizeof(buf));
}

static uint8_t pcd_read_reg(spi_device_handle_t spi, uint8_t reg)
{
    uint8_t buf[2] = { (uint8_t)(0x80 | reg), 0 };
    if (spi_tx_rx(spi, buf, sizeof(buf)) != ESP_OK) {
        return 0;
    }
    return buf[1];
}

static void pcd_write_reg_n(spi_device_handle_t spi, uint8_t reg, uint8_t count, const uint8_t *values)
{
    uint8_t buf[66];
    if (count > sizeof(buf) - 1) {
        count = (uint8_t)(sizeof(buf) - 1);
    }
    buf[0] = reg;
    memcpy(buf + 1, values, count);
    (void)spi_tx_rx(spi, buf, (size_t)(count + 1));
}

static void pcd_read_fifo_bytes(spi_device_handle_t spi, uint8_t n, uint8_t *values, uint8_t rx_align)
{
    if (n == 0) {
        return;
    }
    if (rx_align) {
        uint8_t mask = (uint8_t)((0xFFu << rx_align) & 0xFFu);
        uint8_t b = pcd_read_reg(spi, REG_FIFODataReg);
        values[0] = (uint8_t)(b & mask);
        for (uint8_t i = 1; i < n; i++) {
            values[i] = pcd_read_reg(spi, REG_FIFODataReg);
        }
    } else {
        for (uint8_t i = 0; i < n; i++) {
            values[i] = pcd_read_reg(spi, REG_FIFODataReg);
        }
    }
}

static void pcd_set_reg_mask(spi_device_handle_t spi, uint8_t reg, uint8_t mask)
{
    uint8_t t = pcd_read_reg(spi, reg);
    pcd_write_reg(spi, reg, (uint8_t)(t | mask));
}

static void pcd_clear_reg_mask(spi_device_handle_t spi, uint8_t reg, uint8_t mask)
{
    uint8_t t = pcd_read_reg(spi, reg);
    pcd_write_reg(spi, reg, (uint8_t)(t & (~mask)));
}

static mfrc522_status_t pcd_calc_crc(spi_device_handle_t spi, uint8_t *data, uint8_t length, uint8_t *result)
{
    pcd_write_reg(spi, REG_CommandReg, PCD_Idle);
    pcd_write_reg(spi, REG_DivIrqReg, 0x04);
    pcd_write_reg(spi, REG_FIFOLevelReg, 0x80);
    pcd_write_reg_n(spi, REG_FIFODataReg, length, data);
    pcd_write_reg(spi, REG_CommandReg, PCD_CalcCRC);
    uint32_t deadline = millis() + 89;
    do {
        if (pcd_read_reg(spi, REG_DivIrqReg) & 0x04) {
            pcd_write_reg(spi, REG_CommandReg, PCD_Idle);
            result[0] = pcd_read_reg(spi, REG_CRCResultRegL);
            result[1] = pcd_read_reg(spi, REG_CRCResultRegH);
            return MFRC522_OK;
        }
        vTaskDelay(1);
    } while (millis() < deadline);
    return MFRC522_TIMEOUT;
}

static void pcd_reset(spi_device_handle_t spi)
{
    pcd_write_reg(spi, REG_CommandReg, PCD_SoftReset);
    uint8_t c = 0;
    do {
        vTaskDelay(pdMS_TO_TICKS(50));
    } while ((pcd_read_reg(spi, REG_CommandReg) & (1 << 4)) && (++c) < 3);
}

static void pcd_antenna_on(spi_device_handle_t spi)
{
    uint8_t v = pcd_read_reg(spi, REG_TxControlReg);
    if ((v & 0x03) != 0x03) {
        pcd_write_reg(spi, REG_TxControlReg, (uint8_t)(v | 0x03));
    }
}

static mfrc522_status_t pcd_communicate_with_picc(spi_device_handle_t spi, uint8_t command, uint8_t wait_irq,
                                                  uint8_t *send_data, uint8_t send_len, uint8_t *back_data,
                                                  uint8_t *back_len, uint8_t *valid_bits, uint8_t rx_align,
                                                  bool check_crc)
{
    uint8_t tx_last_bits = valid_bits ? *valid_bits : 0;
    uint8_t bit_framing = (uint8_t)((rx_align << 4) + tx_last_bits);
    pcd_write_reg(spi, REG_CommandReg, PCD_Idle);
    pcd_write_reg(spi, REG_ComIrqReg, 0x7F);
    pcd_write_reg(spi, REG_FIFOLevelReg, 0x80);
    pcd_write_reg_n(spi, REG_FIFODataReg, send_len, send_data);
    pcd_write_reg(spi, REG_BitFramingReg, bit_framing);
    pcd_write_reg(spi, REG_CommandReg, command);
    if (command == PCD_Transceive) {
        pcd_set_reg_mask(spi, REG_BitFramingReg, 0x80);
    }
    /* 36ms đôi khi hụt trên ESP (FreeRTOS); 50ms an toàn hơn với thẻ/clone RC522 */
    uint32_t deadline = millis() + 50;
    bool completed = false;
    do {
        uint8_t n = pcd_read_reg(spi, REG_ComIrqReg);
        if (n & wait_irq) {
            completed = true;
            break;
        }
        if (n & 0x01) {
            return MFRC522_TIMEOUT;
        }
        vTaskDelay(1);
    } while (millis() < deadline);
    if (!completed) {
        return MFRC522_TIMEOUT;
    }
    uint8_t err = pcd_read_reg(spi, REG_ErrorReg);
    if (err & 0x13) {
        return MFRC522_ERROR;
    }
    uint8_t _valid_bits = 0;
    if (back_data && back_len) {
        uint8_t n = pcd_read_reg(spi, REG_FIFOLevelReg);
        if (n > *back_len) {
            return MFRC522_NO_ROOM;
        }
        *back_len = n;
        pcd_read_fifo_bytes(spi, n, back_data, rx_align);
        _valid_bits = (uint8_t)(pcd_read_reg(spi, REG_ControlReg) & 0x07);
        if (valid_bits) {
            *valid_bits = _valid_bits;
        }
    }
    if (err & 0x08) {
        return MFRC522_COLLISION;
    }
    if (back_data && back_len && check_crc) {
        if (*back_len == 1 && _valid_bits == 4) {
            return MFRC522_ERROR;
        }
        if (*back_len < 2 || _valid_bits != 0) {
            return MFRC522_CRC_WRONG;
        }
        uint8_t control[2];
        if (pcd_calc_crc(spi, &back_data[0], (uint8_t)(*back_len - 2), control) != MFRC522_OK) {
            return MFRC522_ERROR;
        }
        if (back_data[*back_len - 2] != control[0] || back_data[*back_len - 1] != control[1]) {
            return MFRC522_CRC_WRONG;
        }
    }
    return MFRC522_OK;
}

static mfrc522_status_t pcd_transceive_data(spi_device_handle_t spi, uint8_t *send_data, uint8_t send_len,
                                            uint8_t *back_data, uint8_t *back_len, uint8_t *valid_bits,
                                            uint8_t rx_align, bool check_crc)
{
    return pcd_communicate_with_picc(spi, PCD_Transceive, 0x30, send_data, send_len, back_data, back_len,
                                     valid_bits, rx_align, check_crc);
}

static mfrc522_status_t picc_reqa_or_wupa(spi_device_handle_t spi, uint8_t cmd, uint8_t *buffer_atqa,
                                          uint8_t *buffer_size)
{
    if (buffer_atqa == NULL || *buffer_size < 2) {
        return MFRC522_NO_ROOM;
    }
    uint8_t valid_bits = 7;
    pcd_clear_reg_mask(spi, REG_CollReg, 0x80);
    mfrc522_status_t st =
        pcd_transceive_data(spi, &cmd, 1, buffer_atqa, buffer_size, &valid_bits, 0, false);
    if (st != MFRC522_OK) {
        return st;
    }
    if (*buffer_size != 2 || valid_bits != 0) {
        return MFRC522_ERROR;
    }
    return MFRC522_OK;
}
static mfrc522_status_t picc_select(spi_device_handle_t spi, mfrc522_uid_t *uid, uint8_t valid_bits_in)
{
    bool uid_complete = false;
    bool select_done;
    bool use_cascade_tag;
    uint8_t cascade_level = 1;
    mfrc522_status_t result;
    uint8_t count;
    uint8_t check_bit;
    uint8_t index;
    uint8_t uid_index;
    int8_t current_level_known_bits;
    uint8_t buffer[9];
    uint8_t buffer_used;
    uint8_t rx_align;
    uint8_t tx_last_bits;
    uint8_t *response_buffer = &buffer[6];
    uint8_t response_length;

    if (valid_bits_in > 80) {
        return MFRC522_INVALID;
    }
    pcd_clear_reg_mask(spi, REG_CollReg, 0x80);

    while (!uid_complete) {
        switch (cascade_level) {
        case 1:
            buffer[0] = PICC_CMD_SEL_CL1;
            uid_index = 0;
            use_cascade_tag = (valid_bits_in != 0) && (uid->size > 4);
            break;
        case 2:
            buffer[0] = 0x95;
            uid_index = 3;
            use_cascade_tag = (valid_bits_in != 0) && (uid->size > 7);
            break;
        case 3:
            buffer[0] = 0x97;
            uid_index = 6;
            use_cascade_tag = false;
            break;
        default:
            return MFRC522_INTERNAL;
        }

        current_level_known_bits = (int8_t)(valid_bits_in - (8 * uid_index));
        if (current_level_known_bits < 0) {
            current_level_known_bits = 0;
        }

        index = 2;
        if (use_cascade_tag) {
            buffer[index++] = PICC_CMD_CT;
        }
        uint8_t bytes_to_copy = (uint8_t)(current_level_known_bits / 8 + (current_level_known_bits % 8 ? 1 : 0));
        if (bytes_to_copy) {
            uint8_t max_bytes = use_cascade_tag ? 3 : 4;
            if (bytes_to_copy > max_bytes) {
                bytes_to_copy = max_bytes;
            }
            for (count = 0; count < bytes_to_copy; count++) {
                buffer[index++] = uid->uid_byte[uid_index + count];
            }
        }
        if (use_cascade_tag) {
            current_level_known_bits += 8;
        }

        select_done = false;
        while (!select_done) {
            if (current_level_known_bits >= 32) {
                buffer[1] = 0x70;
                buffer[6] = (uint8_t)(buffer[2] ^ buffer[3] ^ buffer[4] ^ buffer[5]);
                result = pcd_calc_crc(spi, buffer, 7, &buffer[7]);
                if (result != MFRC522_OK) {
                    return result;
                }
                tx_last_bits = 0;
                buffer_used = 9;
                response_buffer = &buffer[6];
                response_length = 3;
            } else {
                tx_last_bits = (uint8_t)(current_level_known_bits % 8);
                count = (uint8_t)(current_level_known_bits / 8);
                index = (uint8_t)(2 + count);
                buffer[1] = (uint8_t)((index << 4) + tx_last_bits);
                buffer_used = (uint8_t)(index + (tx_last_bits ? 1 : 0));
                response_buffer = &buffer[index];
                response_length = (uint8_t)(sizeof(buffer) - index);
            }

            rx_align = tx_last_bits;
            pcd_write_reg(spi, REG_BitFramingReg, (uint8_t)((rx_align << 4) + tx_last_bits));

            result = pcd_transceive_data(spi, buffer, buffer_used, response_buffer, &response_length, &tx_last_bits,
                                         rx_align, false);
            if (result == MFRC522_COLLISION) {
                uint8_t valueOfCollReg = pcd_read_reg(spi, REG_CollReg);
                if (valueOfCollReg & 0x20) {
                    return MFRC522_COLLISION;
                }
                uint8_t collision_pos = (uint8_t)(valueOfCollReg & 0x1F);
                if (collision_pos == 0) {
                    collision_pos = 32;
                }
                if (collision_pos <= (uint8_t)current_level_known_bits) {
                    return MFRC522_INTERNAL;
                }
                current_level_known_bits = (int8_t)collision_pos;
                count = (uint8_t)(current_level_known_bits % 8);
                check_bit = (uint8_t)((current_level_known_bits - 1) % 8);
                index = (uint8_t)(1 + (current_level_known_bits / 8) + (count ? 1 : 0));
                buffer[index] |= (uint8_t)(1 << check_bit);
            } else if (result != MFRC522_OK) {
                return result;
            } else {
                if (current_level_known_bits >= 32) {
                    select_done = true;
                } else {
                    current_level_known_bits = 32;
                }
            }
        }

        index = (buffer[2] == PICC_CMD_CT) ? 3 : 2;
        uint8_t bytes_to_copy2 = (buffer[2] == PICC_CMD_CT) ? 3 : 4;
        for (count = 0; count < bytes_to_copy2; count++) {
            uid->uid_byte[uid_index + count] = buffer[index++];
        }

        if (response_length != 3 || tx_last_bits != 0) {
            return MFRC522_ERROR;
        }
        result = pcd_calc_crc(spi, response_buffer, 1, &buffer[2]);
        if (result != MFRC522_OK) {
            return result;
        }
        if (buffer[2] != response_buffer[1] || buffer[3] != response_buffer[2]) {
            return MFRC522_CRC_WRONG;
        }
        if (response_buffer[0] & 0x04) {
            cascade_level++;
        } else {
            uid_complete = true;
            uid->sak = response_buffer[0];
        }
    }

    uid->size = (uint8_t)(3 * cascade_level + 1);
    return MFRC522_OK;
}

esp_err_t mfrc522_init(spi_device_handle_t spi, int rst_gpio)
{
    if (rst_gpio >= 0) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << (unsigned)rst_gpio, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level((gpio_num_t)rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level((gpio_num_t)rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else {
        pcd_reset(spi);
    }

    pcd_write_reg(spi, REG_TxModeReg, 0x00);
    pcd_write_reg(spi, REG_RxModeReg, 0x00);
    pcd_write_reg(spi, REG_ModWidthReg, 0x26);
    pcd_write_reg(spi, REG_TModeReg, 0x80);
    pcd_write_reg(spi, REG_TPrescalerReg, 0xA9);
    pcd_write_reg(spi, REG_TReloadRegH, 0x03);
    pcd_write_reg(spi, REG_TReloadRegL, 0xE8);
    pcd_write_reg(spi, REG_TxASKReg, 0x40);
    pcd_write_reg(spi, REG_ModeReg, 0x3D);
    pcd_antenna_on(spi);
    /* Rx gain tối đa 48 dB (0x70) — giúp nhận thẻ nhạy hơn ở khoảng cách xa */
    pcd_write_reg(spi, REG_RFCfgReg, 0x70);
    uint8_t ver = pcd_read_reg(spi, REG_VersionReg);
    uint8_t rfc = pcd_read_reg(spi, REG_RFCfgReg);
    // ESP_LOGI(TAG_MF, "RC522 VersionReg=0x%02x RFCfg=0x%02x (sau khi ghi 0x70/48dB vao RxGain)", ver, rfc);
    // if (ver == 0x00 || ver == 0xFF) {
    //     //ESP_LOGW(TAG_MF,
    //              "VersionReg 0x%02x: kiem tra day SPI (MISO doc duoc), CS/RST, nguon 3.3V RC522; sdspi khong chiem MISO khi CS_SD=1.",
    //              ver);
    // } else if (ver != 0x91 && ver != 0x92 && ver != 0x12) {
    //     //ESP_LOGW(TAG_MF,
    //              "VersionReg 0x%02x khac 91/92/12 — thu xem log sau khi sua SPI 1-byte/CS; van loi thi MISO/CS/clone chip.",
    //              ver);
    // }
    // if ((rfc & 0xE0) != 0x40) {
    //     /* RFCfg gain field thường 0b010xxxxx sau khi ghi 0x48 */
    //     //ESP_LOGW(TAG_MF, "RFCfg=0x%02x: neu khong gan 0x4x, SPI doc RC522 co the van sai.", rfc);
    // }
    return ESP_OK;
}

const char *mfrc522_status_name(mfrc522_status_t s)
{
    switch (s) {
    case MFRC522_OK:
        return "OK";
    case MFRC522_ERROR:
        return "ERROR";
    case MFRC522_COLLISION:
        return "COLLISION";
    case MFRC522_TIMEOUT:
        return "TIMEOUT";
    case MFRC522_NO_ROOM:
        return "NO_ROOM";
    case MFRC522_INTERNAL:
        return "INTERNAL";
    case MFRC522_INVALID:
        return "INVALID";
    case MFRC522_CRC_WRONG:
        return "CRC_WRONG";
    default:
        return "?";
    }
}

esp_err_t mfrc522_init_bitbang(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso, gpio_num_t cs, int rst_gpio)
{
    s_bb_mode = true;
    s_bb_sck = sck;
    s_bb_mosi = mosi;
    s_bb_miso = miso;
    s_bb_cs = cs;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << sck) | (1ULL << mosi) | (1ULL << cs),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << miso,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
    gpio_set_level(cs, 1);
    gpio_set_level(sck, 0);
    gpio_set_level(mosi, 0);

    return mfrc522_init(MFRC522_HANDLE_BITBANG, rst_gpio);
}

spi_device_handle_t mfrc522_spi(void)
{
    return s_bb_mode ? MFRC522_HANDLE_BITBANG : s_hw_spi;
}

esp_err_t mfrc522_init_spi_device(spi_device_handle_t spi, int rst_gpio)
{
    s_bb_mode = false;
    s_hw_spi = spi;
    return mfrc522_init(spi, rst_gpio);
}

mfrc522_status_t mfrc522_picc_is_new_card_present(spi_device_handle_t spi)
{
    pcd_write_reg(spi, REG_TxModeReg, 0x00);
    pcd_write_reg(spi, REG_RxModeReg, 0x00);
    pcd_write_reg(spi, REG_ModWidthReg, 0x26);
    uint8_t buffer_atqa[2];
    uint8_t buffer_size = sizeof(buffer_atqa);
    mfrc522_status_t r = picc_reqa_or_wupa(spi, PICC_CMD_REQA, buffer_atqa, &buffer_size);
    if (r == MFRC522_OK || r == MFRC522_COLLISION) {
        return MFRC522_OK;
    }
    /* Thẻ có thể ở HALT — thử WUPA (0x52) như thư viện Arduino thường làm sau REQA lỗi */
    buffer_size = sizeof(buffer_atqa);
    r = picc_reqa_or_wupa(spi, PICC_CMD_WUPA, buffer_atqa, &buffer_size);
    if (r == MFRC522_OK || r == MFRC522_COLLISION) {
        return MFRC522_OK;
    }
    return r;
}

mfrc522_status_t mfrc522_picc_read_card_serial(spi_device_handle_t spi, mfrc522_uid_t *uid)
{
    memset(uid, 0, sizeof(*uid));
    return picc_select(spi, uid, 0);
}
