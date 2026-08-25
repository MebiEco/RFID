#pragma once

#include "board_pins.h"
#include "esp_err.h"

#define BOARD_SD_WAV_FILENAME  "121.wav"
#define BOARD_SD_WAV_PATH      BOARD_SD_MOUNT_POINT "/" BOARD_SD_WAV_FILENAME

esp_err_t sd_wav_play(void);
