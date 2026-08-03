#include "ds3231.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "ds3231";

#if BOARD_ENABLE_DS3231

#define DS3231_REG_TIME 0x00
#define DS3231_REG_STATUS 0x0F
#define DS3231_STAT_OSF (1u << 7)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10u) + (v & 0x0Fu));
}

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static esp_err_t ds3231_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_dev || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 200);
}

static esp_err_t ds3231_write_regs(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!s_dev || !buf || len == 0 || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tmp[17];
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 200);
}

/** mktime theo UTC thuần (không phụ thuộc TZ local UTC-7). */
static time_t tm_fields_as_utc(struct tm *t)
{
    t->tm_isdst = 0;
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t utc = mktime(t);
    setenv("TZ", "UTC-7", 1);
    tzset();
    return utc;
}

esp_err_t ds3231_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_DS3231_I2C_PORT,
        .sda_io_num = BOARD_DS3231_SDA_GPIO,
        .scl_io_num = BOARD_DS3231_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus fail: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_DS3231_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Add device fail: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    uint8_t status = 0;
    err = ds3231_read_regs(DS3231_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Probe DS3231 fail (SDA=%d SCL=%d): %s — dung NTP/NVS",
                 (int)BOARD_DS3231_SDA_GPIO, (int)BOARD_DS3231_SCL_GPIO, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "DS3231 OK (I2C SDA=%d SCL=%d, status=0x%02X)", (int)BOARD_DS3231_SDA_GPIO,
             (int)BOARD_DS3231_SCL_GPIO, (unsigned)status);
    return ESP_OK;
}

bool ds3231_is_ready(void)
{
    return s_ready;
}

esp_err_t ds3231_get_utc(time_t *utc_out)
{
    if (!utc_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    esp_err_t err = ds3231_read_regs(DS3231_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (status & DS3231_STAT_OSF) {
        ESP_LOGW(TAG, "OSF=1 (mat pin / chua set) — bo qua RTC");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7];
    err = ds3231_read_regs(DS3231_REG_TIME, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = bcd2bin(raw[0] & 0x7F);
    t.tm_min = bcd2bin(raw[1] & 0x7F);
    t.tm_hour = bcd2bin(raw[2] & 0x3F); /* 24h */
    t.tm_mday = bcd2bin(raw[4] & 0x3F);
    t.tm_mon = bcd2bin(raw[5] & 0x1F) - 1;
    t.tm_year = bcd2bin(raw[6]) + 100; /* 2000+ */

    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31 || t.tm_year < (2020 - 1900)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    time_t utc = tm_fields_as_utc(&t);
    if (utc == (time_t)-1) {
        return ESP_FAIL;
    }
    *utc_out = utc;
    return ESP_OK;
}

esp_err_t ds3231_set_utc(time_t utc)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (utc < 1577836800LL) { /* 2020-01-01 */
        return ESP_ERR_INVALID_ARG;
    }

    struct tm t;
    gmtime_r(&utc, &t);

    uint8_t raw[7];
    raw[0] = bin2bcd((uint8_t)t.tm_sec);
    raw[1] = bin2bcd((uint8_t)t.tm_min);
    raw[2] = bin2bcd((uint8_t)t.tm_hour); /* 24h, bit6=0 */
    raw[3] = bin2bcd((uint8_t)(t.tm_wday == 0 ? 7 : t.tm_wday));
    raw[4] = bin2bcd((uint8_t)t.tm_mday);
    raw[5] = bin2bcd((uint8_t)(t.tm_mon + 1));
    raw[6] = bin2bcd((uint8_t)(t.tm_year % 100));

    esp_err_t err = ds3231_write_regs(DS3231_REG_TIME, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status = 0;
    err = ds3231_read_regs(DS3231_REG_STATUS, &status, 1);
    if (err == ESP_OK && (status & DS3231_STAT_OSF)) {
        status = (uint8_t)(status & (uint8_t)~DS3231_STAT_OSF);
        (void)ds3231_write_regs(DS3231_REG_STATUS, &status, 1);
    }
    return ESP_OK;
}

#else /* !BOARD_ENABLE_DS3231 */

esp_err_t ds3231_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ds3231_is_ready(void)
{
    return false;
}

esp_err_t ds3231_get_utc(time_t *utc_out)
{
    (void)utc_out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ds3231_set_utc(time_t utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BOARD_ENABLE_DS3231 */
