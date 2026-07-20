#pragma once

#include "esp_err.h"

/** Khởi tạo task âm thanh + hàng đợi (gọi một lần sau khi NVS/init cơ bản). */
void app_audio_start(void);

/**
 * Đưa đường dẫn WAV vào hàng đợi phát (PCM 16-bit, mono hoặc stereo).
 * Không block; SD phải đã mount nếu path nằm trên /sdcard.
 */
esp_err_t app_audio_queue_wav(const char *path);

/** 4.wav — xác nhận lệnh thành công (LCD / web / Azure Direct Method). */
void app_audio_play_confirm(void);

#if BOARD_ENABLE_AUDIO && BOARD_AUDIO_STRESS_TEST
/** Gọi sau app_audio_start + SD mount: xếp hàng 1/2/3.wav nếu tồn tại. */
void app_audio_stress_queue_all_three(void);
#endif

/**
 * Dừng phát ngay (abort + xả I2S im lặng + tắt DMA). Gọi trước khi rfid_task
 * cần khóa SD — tránh loa giữ mẫu PCM cuối (tiếng "nnn"/"xin xin" lặp).
 */
void app_audio_pause(void);

/** Xóa WAV đang chờ trong hàng đợi (không abort bài đang phát). */
void app_audio_clear_queue(void);

/** Dừng phát + xóa hàng đợi — dùng trước khi xếp 2/3.wav mới sau quẹt thẻ. */
void app_audio_stop_and_clear(void);

/** Giữ tương thích — hiện không cần gọi sau pause (I2S tự bật lại khi phát tiếp). */
void app_audio_resume(void);

/** Lay/Chinh am luong (%) (0-100) */
void app_audio_set_volume(uint8_t vol_pct);
uint8_t app_audio_get_volume(void);
