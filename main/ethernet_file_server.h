#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize W5500 Ethernet using a static IPv4 address.
 *
 * The SD card must already be mounted at /sdcard.
 */
esp_err_t ethernet_file_server_start(void);

#ifdef __cplusplus
}
#endif