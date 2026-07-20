#include "app_audio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_check.h"
#include "esp_log.h"

#include "driver/i2s_std.h"

#include "board_pins.h"
#include "sd_card.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "app_audio";

static uint8_t s_vol_pct = 100;

void app_audio_set_volume(uint8_t vol_pct)
{
    if (vol_pct > 100) vol_pct = 100;
    s_vol_pct = vol_pct;
    nvs_handle_t h;
    if (nvs_open("wifi_portal", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol_pct", s_vol_pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint8_t app_audio_get_volume(void)
{
    return s_vol_pct;
}

#define AUDIO_QUEUE_DEPTH   4
/* Stack: play_wav — raw lớn + FatFS/stdio; tăng lên 16K+ để đẩy sang PSRAM */
#define AUDIO_TASK_STACK    16384

/**
 * Đọc mỗi lần từ SD (byte PCM).
 * Mono: 2048 B = 1024 mẫu → 2048 int16 stereo. Stereo: 4096 B = 1024 frame LR → stereo[2048].
 */
#define RAW_PCM_MAX_BYTES 4096
/** Tối đa mẫu stereo ra I2S mỗi chunk (mono 1024 mẫu → 2048 halfword). */
#define STEREO_SAMPLES_MAX 2048

typedef struct {
    char path[128];
} audio_msg_t;

static QueueHandle_t s_audio_q;
static i2s_chan_handle_t s_tx_chan;
static uint32_t s_open_rate_hz;

/*
 * Bộ đệm PCM cố định — tránh malloc(MALLOC_CAP_INTERNAL) khi heap nội đã căng
 * (WiFi/LCD/Azure) gây "Het RAM dem PCM". Task audio chỉ xử lý tuần tự một file.
 */
/* Bộ đệm PCM cố định — chuyển sang SPIRAM để tiết kiệm 8KB RAM nội bộ. */
static EXT_RAM_ATTR uint8_t s_pcm_raw[RAW_PCM_MAX_BYTES];
static EXT_RAM_ATTR int16_t s_pcm_stereo[STEREO_SAMPLES_MAX];

static void audio_i2s_teardown(void)
{
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_open_rate_hz = 0;
    }
}

volatile bool g_audio_abort = false;

/** Ghi im lặng vài chunk để thay mẫu PCM cuối trong DMA — tránh rè "nnn". */
static void audio_i2s_flush_silence(void)
{
    if (!s_tx_chan) {
        return;
    }
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    for (int i = 0; i < 6; i++) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, silence, sizeof(silence), &written, pdMS_TO_TICKS(20));
        if (err != ESP_OK) {
            break;
        }
    }
}

static void audio_abort_teardown(void)
{
    audio_i2s_flush_silence();
    audio_i2s_teardown();
}

/**
 * Ra hiệu dừng phát nhạc ngay lập tức.
 * Gọi từ rfid_task khi quẹt thẻ để nhường quyền truy cập SD.
 */
void app_audio_pause(void)
{
    g_audio_abort = true;
}

void app_audio_clear_queue(void)
{
    if (s_audio_q) {
        xQueueReset(s_audio_q);
    }
}

void app_audio_stop_and_clear(void)
{
    g_audio_abort = true;
    app_audio_clear_queue();
}

/** Giữ tương thích — I2S được bật lại trong play_wav_file khi phát tiếp. */
void app_audio_resume(void)
{
}

static esp_err_t audio_i2s_prepare(uint32_t sample_rate_hz)
{
    if (sample_rate_hz < 8000u || sample_rate_hz > 48000u) {
        ESP_LOGE(TAG, "Sample rate khong ho tro: %" PRIu32, sample_rate_hz);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tx_chan && s_open_rate_hz == sample_rate_hz) {
        return ESP_OK;
    }

    audio_i2s_teardown();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;
    /* ~12 * 512 frame stereo 16-bit: đệm sâu hơn trước lúc hụt khi SD/WiFi tranh tài nguyên */
    chan_cfg.dma_frame_num = 512;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        audio_i2s_teardown();
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        audio_i2s_teardown();
        return err;
    }

    s_open_rate_hz = sample_rate_hz;
    ESP_LOGI(TAG, "I2S TX %" PRIu32 " Hz (BCLK=%d WS=%d DOUT=%d)", sample_rate_hz,
             (int)BOARD_I2S_BCLK_GPIO, (int)BOARD_I2S_WS_GPIO, (int)BOARD_I2S_DOUT_GPIO);
    return ESP_OK;
}

/**
 * Đọc WAV PCM; trả về offset/lenth của chunk data để stream (đặt file về đầu file trước khi gọi).
 */
static esp_err_t wav_parse(FILE *f, uint32_t *rate_hz, uint16_t *channels, uint16_t *bits,
                           long *data_offset, size_t *data_len)
{
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12) {
        return ESP_FAIL;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Khong phai WAV RIFF");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    long file_len = ftell(f);
    if (file_len <= 12) {
        return ESP_FAIL;
    }
    if (fseek(f, 12, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    uint32_t sample_rate = 0;
    uint16_t num_ch = 0;
    uint16_t bits_per = 0;
    long data_off = -1;
    size_t data_bytes = 0;

    while (ftell(f) + 8 <= file_len) {
        char chunk_id[4];
        uint32_t chunk_sz;
        if (fread(chunk_id, 1, 4, f) != 4) {
            break;
        }
        if (fread(&chunk_sz, 4, 1, f) != 1) {
            break;
        }

        long payload_start = ftell(f);

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_sz < 16) {
                fseek(f, chunk_sz, SEEK_CUR);
            } else {
                uint8_t fmt[48];
                size_t n = chunk_sz > sizeof(fmt) ? sizeof(fmt) : chunk_sz;
                if (fread(fmt, 1, n, f) != n) {
                    return ESP_FAIL;
                }
                uint16_t audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
                if (audio_format != 1u) {
                    ESP_LOGE(TAG, "Chi ho tro PCM linear (fmt=%u)", (unsigned)audio_format);
                    return ESP_ERR_NOT_SUPPORTED;
                }
                num_ch = (uint16_t)(fmt[2] | (fmt[3] << 8));
                sample_rate = (uint32_t)(fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24));
                bits_per = (uint16_t)(fmt[14] | (fmt[15] << 8));
                if (chunk_sz > n) {
                    fseek(f, (long)(payload_start + chunk_sz), SEEK_SET);
                }
            }
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_off = payload_start;
            data_bytes = chunk_sz;
            fseek(f, chunk_sz, SEEK_CUR);
        } else {
            fseek(f, chunk_sz, SEEK_CUR);
        }

        if (chunk_sz & 1u) {
            fseek(f, 1, SEEK_CUR);
        }

        if (sample_rate && num_ch && bits_per && data_off >= 0) {
            break;
        }
    }

    if (sample_rate == 0 || num_ch == 0 || bits_per == 0 || data_off < 0 || data_bytes == 0) {
        ESP_LOGE(TAG, "Thieu fmt/data trong WAV");
        return ESP_ERR_INVALID_SIZE;
    }

    if (bits_per != 16u) {
        ESP_LOGE(TAG, "Chi ho tro 16-bit (co %u)", (unsigned)bits_per);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (num_ch != 1u && num_ch != 2u) {
        ESP_LOGE(TAG, "Chi ho tro 1 hoac 2 kenh (%u)", (unsigned)num_ch);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *rate_hz = sample_rate;
    *channels = num_ch;
    *bits = bits_per;
    *data_offset = data_off;
    *data_len = data_bytes;
    return ESP_OK;
}

static esp_err_t play_wav_file(const char *path)
{
    /* Chỉ khóa khi dùng FILE* trên thẻ; tách khỏi i2s_channel_write (dài) để RFID/ghi
     * log không chặn cả bài. */
    sd_card_lock();
    FILE *f = fopen(path, "rb");
    if (!f) {
        sd_card_unlock();
        ESP_LOGW(TAG, "Mo file loi: %s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }

    uint32_t rate_hz;
    uint16_t ch;
    uint16_t bits;
    long data_off;
    size_t data_len;

    esp_err_t err = wav_parse(f, &rate_hz, &ch, &bits, &data_off, &data_len);
    if (err != ESP_OK) {
        fclose(f);
        sd_card_unlock();
        return err;
    }

    if (fseek(f, data_off, SEEK_SET) != 0) {
        fclose(f);
        sd_card_unlock();
        return ESP_FAIL;
    }

    err = audio_i2s_prepare(rate_hz);
    if (err != ESP_OK) {
        fclose(f);
        sd_card_unlock();
        return err;
    }

    const size_t frame_bytes = (size_t)(bits / 8u) * (size_t)ch;
    uint8_t *raw = s_pcm_raw;
    int16_t *stereo = s_pcm_stereo;

    size_t remaining = data_len;
    esp_err_t out_err = ESP_OK;
    while (remaining > 0) {
        if (g_audio_abort) {
            break;
        }

        size_t nread = remaining > RAW_PCM_MAX_BYTES ? RAW_PCM_MAX_BYTES : remaining;
        /* Mono: 2048 B/đợt = 2× chunk cũ; stereo: 4096 B/đợt */
        if (ch == 1u) {
            if (nread > 2048u) {
                nread = 2048u;
            }
        } else {
            if (nread > 4096u) {
                nread = 4096u;
            }
        }
        nread -= nread % frame_bytes;
        if (nread == 0) {
            break;
        }

        size_t got = fread(raw, 1, nread, f);
        if (got == 0) {
            break;
        }
        got -= got % frame_bytes;

        size_t samples_in = got / frame_bytes;
        size_t out_bytes;

        if (ch == 1u && samples_in > (STEREO_SAMPLES_MAX / 2u)) {
            ESP_LOGE(TAG, "PCM mono vuot dem tinh (%u)", (unsigned)samples_in);
            out_err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (ch != 1u && samples_in * 2u > STEREO_SAMPLES_MAX) {
            ESP_LOGE(TAG, "PCM stereo vuot dem tinh (%u)", (unsigned)samples_in);
            out_err = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (ch == 1u) {
            const int16_t *mono = (const int16_t *)(const void *)raw;
            for (size_t i = 0; i < samples_in; i++) {
                int32_t temp = ((int32_t)mono[i] * (int32_t)BOARD_AUDIO_PCM_GAIN_NUM * s_vol_pct) /
                               ((int32_t)BOARD_AUDIO_PCM_GAIN_DEN * 100);
                if (temp > 32767) {
                    temp = 32767;
                }
                if (temp < -32768) {
                    temp = -32768;
                }
                int16_t val = (int16_t)temp;
                stereo[i * 2u] = val;
                stereo[i * 2u + 1u] = val;
            }
            out_bytes = samples_in * 2u * sizeof(int16_t);
        } else {
            const int16_t *src = (const int16_t *)(const void *)raw;
            for (size_t i = 0; i < samples_in * 2u; i++) {
                int32_t temp = ((int32_t)src[i] * (int32_t)BOARD_AUDIO_PCM_GAIN_NUM * s_vol_pct) /
                               ((int32_t)BOARD_AUDIO_PCM_GAIN_DEN * 100);
                if (temp > 32767) {
                    temp = 32767;
                }
                if (temp < -32768) {
                    temp = -32768;
                }
                stereo[i] = (int16_t)temp;
            }
            out_bytes = got;
        }

        size_t written = 0;
        err = i2s_channel_write(s_tx_chan, stereo, out_bytes, &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write: %s", esp_err_to_name(err));
            out_err = err;
            break;
        }

        remaining -= got;
        if (got < nread) {
            break;
        }
    }

    fclose(f);
    sd_card_unlock();
    if (g_audio_abort) {
        audio_abort_teardown();
    }
    return out_err;
}

static void app_audio_task(void *arg)
{
    (void)arg;
    audio_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_audio_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "Phat: %s", msg.path);
        g_audio_abort = false;
        (void)play_wav_file(msg.path);

        /* Log stack watermark sau mỗi 5 lần phát */
        // if (++s_play_count % 5 == 0) {
        //     UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
        //     ESP_LOGW(TAG, "[STACK] audio_task free: %4u words (%5u bytes) / 12288 total",
        //              (unsigned)wm, (unsigned)(wm * sizeof(StackType_t)));
        //     if (wm * sizeof(StackType_t) < 512) {
        //         ESP_LOGE(TAG, "[STACK] audio_task SAP STACK OVERFLOW!");
        //     }
        // }
    }
}

void app_audio_start(void)
{
    if (s_audio_q) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("wifi_portal", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "vol_pct", &s_vol_pct);
        nvs_close(h);
    }
    s_audio_q = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_msg_t));
    if (!s_audio_q) {
        ESP_LOGE(TAG, "xQueueCreate thất bại");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(app_audio_task, "app_audio", AUDIO_TASK_STACK, NULL, BOARD_AUDIO_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Khong tao duoc task app_audio");
        vQueueDelete(s_audio_q);
        s_audio_q = NULL;
        return;
    }
    ESP_LOGI(TAG, "San sang I2S TX — BCLK=%d LRC=%d DIN=%d | loa %dΩ %.1fW gain %d/%d",
             (int)BOARD_I2S_BCLK_GPIO, (int)BOARD_I2S_WS_GPIO, (int)BOARD_I2S_DOUT_GPIO,
             BOARD_SPEAKER_OHM, (float)BOARD_SPEAKER_POWER_W_x10 / 10.f, BOARD_AUDIO_PCM_GAIN_NUM,
             BOARD_AUDIO_PCM_GAIN_DEN);
}

esp_err_t app_audio_queue_wav(const char *path)
{
    if (!s_audio_q) {
        ESP_LOGW(TAG, "app_audio_start() chua goi");
        return ESP_ERR_INVALID_STATE;
    }
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    snprintf(msg.path, sizeof(msg.path), "%s", path);

    if (xQueueSend(s_audio_q, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Hang doi day, bo qua");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_audio_play_confirm(void)
{
#if BOARD_ENABLE_AUDIO
    (void)app_audio_queue_wav(BOARD_SD_AUDIO_4_WAV);
#else
    (void)0;
#endif
}

#if BOARD_ENABLE_AUDIO && BOARD_AUDIO_STRESS_TEST
#include <sys/stat.h>

void app_audio_stress_queue_all_three(void)
{
    if (!s_audio_q) {
        ESP_LOGW(TAG, "stress: app_audio chua start");
        return;
    }
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "stress: SD chua mount");
        return;
    }
    static const char *const paths[3] = {
        BOARD_SD_AUDIO_1_WAV,
        BOARD_SD_AUDIO_2_WAV,
        BOARD_SD_AUDIO_3_WAV,
    };
    for (size_t i = 0; i < 3; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            ESP_LOGW(TAG, "stress: khong co tren the: %s", paths[i]);
            continue;
        }
        esp_err_t e = app_audio_queue_wav(paths[i]);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "stress: queue %s: %s", paths[i], esp_err_to_name(e));
        } else {
            ESP_LOGI(TAG, "stress: xep hang %s", paths[i]);
        }
    }
    ESP_LOGI(TAG, "STRESS: da xep hang 1+2+3 (cac file co tren the)");
}
#endif
