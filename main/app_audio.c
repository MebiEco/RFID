#include "app_audio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

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
#define AUDIO_TASK_STACK    16384
/** I2S luon 48 kHz — ESP32-S3 clock on dinh; 44.1 file se resample. */
#define AUDIO_OUT_RATE_HZ   48000u
/** Chunk Internal day DMA (stereo int16). */
#define STEREO_SAMPLES_MAX  4096
/** Raw WAV preload (PSRAM); convert stereo 48k co the ~2x. */
#define AUDIO_PRELOAD_MAX_BYTES (512 * 1024)

typedef struct {
    char path[128];
} audio_msg_t;

static QueueHandle_t s_audio_q;
static i2s_chan_handle_t s_tx_chan;
static uint32_t s_open_rate_hz;

static DRAM_ATTR int16_t s_pcm_stereo[STEREO_SAMPLES_MAX] __attribute__((aligned(4)));

static void audio_i2s_teardown(void)
{
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_open_rate_hz = 0;
    }
}

/** DMA descriptor phải nằm trong internal DMA-capable RAM (không dùng được PSRAM). */
static esp_err_t audio_i2s_init_channel(uint32_t sample_rate_hz, int dma_desc_num, int dma_frame_num)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = dma_desc_num;
    chan_cfg.dma_frame_num = dma_frame_num;
    /* Underrun → silence thay vi lap mau cu (tieng "nnn"/vap). */
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        return err;
    }

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

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        audio_i2s_teardown();
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        audio_i2s_teardown();
        return err;
    }
    return ESP_OK;
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
    /* Chi xa silence — giu I2S 48 kHz de lan sau khong re-init. */
    audio_i2s_flush_silence();
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
    (void)sample_rate_hz;
    /* Luon 48 kHz — tranh doi clock 44.1↔48 (nghe cham/kho chiu). */
    if (s_tx_chan && s_open_rate_hz == AUDIO_OUT_RATE_HZ) {
        return ESP_OK;
    }

    audio_i2s_teardown();

    /* 8×960 ≈ 30KB DMA (~160 ms @48k stereo). */
    esp_err_t err = audio_i2s_init_channel(AUDIO_OUT_RATE_HZ, 8, 960);
    if (err == ESP_ERR_NO_MEM) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        ESP_LOGW(TAG, "I2S DMA 8x960 fail (largest=%u) — thu 8x512", (unsigned)largest);
        err = audio_i2s_init_channel(AUDIO_OUT_RATE_HZ, 8, 512);
    }
    if (err == ESP_ERR_NO_MEM) {
        err = audio_i2s_init_channel(AUDIO_OUT_RATE_HZ, 6, 240);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init: %s (free_internal=%u largest_dma=%u)", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return err;
    }

    s_open_rate_hz = AUDIO_OUT_RATE_HZ;
    ESP_LOGI(TAG, "I2S TX %" PRIu32 " Hz fixed (BCLK=%d WS=%d DOUT=%d)", AUDIO_OUT_RATE_HZ,
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

static inline int16_t audio_apply_gain(int16_t s)
{
    int32_t t = ((int32_t)s * (int32_t)BOARD_AUDIO_PCM_GAIN_NUM * (int32_t)s_vol_pct) /
                ((int32_t)BOARD_AUDIO_PCM_GAIN_DEN * 100);
    if (t > 32767) {
        t = 32767;
    }
    if (t < -32768) {
        t = -32768;
    }
    return (int16_t)t;
}

static int16_t audio_src_at(const int16_t *pcm, size_t nframes, uint16_t ch, size_t idx, int which)
{
    if (nframes == 0) {
        return 0;
    }
    if (idx >= nframes) {
        idx = nframes - 1;
    }
    if (ch == 1u) {
        return pcm[idx];
    }
    return pcm[idx * 2u + (size_t)which];
}

/** Convert PCM 16-bit → stereo int16 @ 48 kHz (PSRAM). */
static esp_err_t audio_convert_to_48k_stereo(const uint8_t *src, size_t src_bytes, uint32_t src_rate,
                                             uint16_t src_ch, int16_t **out_stereo, size_t *out_bytes)
{
    if (!src || src_rate < 8000u || src_rate > 48000u || (src_ch != 1u && src_ch != 2u)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t src_frames = src_bytes / (sizeof(int16_t) * (size_t)src_ch);
    if (src_frames == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t out_frames =
        (size_t)(((uint64_t)src_frames * (uint64_t)AUDIO_OUT_RATE_HZ + (uint64_t)src_rate / 2u) / (uint64_t)src_rate);
    if (out_frames == 0) {
        out_frames = 1;
    }
    const size_t nbytes = out_frames * 2u * sizeof(int16_t);
    int16_t *dst = (int16_t *)heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) {
        dst = (int16_t *)malloc(nbytes);
    }
    if (!dst) {
        return ESP_ERR_NO_MEM;
    }

    const int16_t *pcm = (const int16_t *)(const void *)src;
    if (src_rate == AUDIO_OUT_RATE_HZ) {
        for (size_t i = 0; i < src_frames; i++) {
            int16_t L = audio_apply_gain(src_ch == 1u ? pcm[i] : pcm[i * 2u]);
            int16_t R = audio_apply_gain(src_ch == 1u ? pcm[i] : pcm[i * 2u + 1u]);
            dst[i * 2u] = L;
            dst[i * 2u + 1u] = R;
        }
    } else {
        uint64_t pos = 0;
        const uint64_t step = ((uint64_t)src_rate << 32) / (uint64_t)AUDIO_OUT_RATE_HZ;
        for (size_t i = 0; i < out_frames; i++) {
            size_t idx = (size_t)(pos >> 32);
            uint32_t frac = (uint32_t)((pos >> 16) & 0xffffu);
            size_t idx2 = (idx + 1u < src_frames) ? (idx + 1u) : idx;
            int32_t l0 = audio_src_at(pcm, src_frames, src_ch, idx, 0);
            int32_t l1 = audio_src_at(pcm, src_frames, src_ch, idx2, 0);
            int32_t r0 = (src_ch == 1u) ? l0 : audio_src_at(pcm, src_frames, src_ch, idx, 1);
            int32_t r1 = (src_ch == 1u) ? l1 : audio_src_at(pcm, src_frames, src_ch, idx2, 1);
            int32_t L = l0 + (((l1 - l0) * (int32_t)frac) >> 16);
            int32_t R = r0 + (((r1 - r0) * (int32_t)frac) >> 16);
            dst[i * 2u] = audio_apply_gain((int16_t)L);
            dst[i * 2u + 1u] = audio_apply_gain((int16_t)R);
            pos += step;
        }
    }

    *out_stereo = dst;
    *out_bytes = nbytes;
    return ESP_OK;
}

/** Day stereo 48k tu PSRAM → Internal chunk → I2S DMA. */
static esp_err_t audio_feed_stereo_48k(const int16_t *stereo, size_t nbytes)
{
    size_t off = 0;
    while (off < nbytes) {
        if (g_audio_abort) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t n = nbytes - off;
        const size_t max_b = sizeof(s_pcm_stereo);
        if (n > max_b) {
            n = max_b;
        }
        n &= ~(size_t)3u;
        if (n == 0) {
            break;
        }
        memcpy(s_pcm_stereo, (const uint8_t *)stereo + off, n);
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx_chan, s_pcm_stereo, n, &written, portMAX_DELAY);
        if (e != ESP_OK) {
            return e;
        }
        off += n;
    }
    return ESP_OK;
}

static esp_err_t play_wav_file(const char *path)
{
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
    if (bits != 16u || fseek(f, data_off, SEEK_SET) != 0) {
        fclose(f);
        sd_card_unlock();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WAV %s: %" PRIu32 " Hz → %" PRIu32 " Hz out, %u ch, PCM %u B", path, rate_hz, AUDIO_OUT_RATE_HZ,
             (unsigned)ch, (unsigned)data_len);

    if (data_len == 0 || data_len > AUDIO_PRELOAD_MAX_BYTES) {
        fclose(f);
        sd_card_unlock();
        ESP_LOGE(TAG, "File qua lon/rong (%u) — can <= %u B", (unsigned)data_len, (unsigned)AUDIO_PRELOAD_MAX_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *preload = (uint8_t *)heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preload) {
        preload = (uint8_t *)malloc(data_len);
    }
    if (!preload) {
        fclose(f);
        sd_card_unlock();
        ESP_LOGE(TAG, "Het RAM preload");
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(preload, 1, data_len, f);
    fclose(f);
    f = NULL;
    sd_card_unlock();

    const size_t frame_bytes = sizeof(int16_t) * (size_t)ch;
    if (got < frame_bytes) {
        heap_caps_free(preload);
        return ESP_FAIL;
    }
    got -= got % frame_bytes;
    ESP_LOGI(TAG, "Preload %u B — convert+phat @%" PRIu32, (unsigned)got, AUDIO_OUT_RATE_HZ);

    int16_t *stereo48 = NULL;
    size_t stereo_bytes = 0;
    err = audio_convert_to_48k_stereo(preload, got, rate_hz, ch, &stereo48, &stereo_bytes);
    heap_caps_free(preload);
    preload = NULL;
    if (err != ESP_OK || !stereo48) {
        ESP_LOGE(TAG, "Convert 48k fail: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = audio_i2s_prepare(AUDIO_OUT_RATE_HZ);
    if (err != ESP_OK) {
        heap_caps_free(stereo48);
        return err;
    }

    esp_err_t out_err = audio_feed_stereo_48k(stereo48, stereo_bytes);
    heap_caps_free(stereo48);

    if (out_err != ESP_OK && out_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2s feed: %s", esp_err_to_name(out_err));
    }
    if (g_audio_abort) {
        audio_abort_teardown();
    }
    return (out_err == ESP_ERR_INVALID_STATE) ? ESP_OK : out_err;
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
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(app_audio_task, "app_audio", AUDIO_TASK_STACK, NULL,
                                                    BOARD_AUDIO_TASK_PRIO, NULL, 1,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(app_audio_task, "app_audio", AUDIO_TASK_STACK, NULL,
                                            BOARD_AUDIO_TASK_PRIO, NULL, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCore(app_audio_task, "app_audio", AUDIO_TASK_STACK, NULL, BOARD_AUDIO_TASK_PRIO, NULL, 1);
    }
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

bool app_audio_is_queue_full(void)
{
    if (!s_audio_q) {
        return false;
    }
    return uxQueueSpacesAvailable(s_audio_q) == 0;
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
