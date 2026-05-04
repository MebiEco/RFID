#pragma once

#include "esp_err.h"

/** Khởi tạo task âm thanh + hàng đợi (gọi một lần sau khi NVS/init cơ bản). */
void app_audio_start(void);

/**
 * Đưa đường dẫn WAV vào hàng đợi phát (PCM 16-bit, mono hoặc stereo).
 * Không block; SD phải đã mount nếu path nằm trên /sdcard.
 */
esp_err_t app_audio_queue_wav(const char *path);

#if BOARD_ENABLE_AUDIO && BOARD_AUDIO_STRESS_TEST
/** Gọi sau app_audio_start + SD mount: xếp hàng 1/2/3.wav nếu tồn tại. */
void app_audio_stress_queue_all_three(void);
#endif

/**
 * Dừng I2S channel (disable DMA) trước khi ghi SD — tránh amp kéo dòng
 * trong lúc SPI đang ghi FAT. Gọi app_audio_resume() sau khi ghi xong.
 */
void app_audio_pause(void);
void app_audio_resume(void);
