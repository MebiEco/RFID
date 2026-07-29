#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  COMMAND_STATUS_OK = 200,
  COMMAND_STATUS_BAD_REQUEST = 400,
  COMMAND_STATUS_NOT_FOUND = 404,
  COMMAND_STATUS_TOO_MANY_REQUEST = 429,
  COMMAND_STATUS_DEVICE_ERROR = 500
} CommandStatus_t;

#ifdef __cplusplus
}
#endif
