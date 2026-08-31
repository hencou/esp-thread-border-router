/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update (OTA) endpoints of the border router web server
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report the running firmware and the progress of an ongoing update.
 */
esp_err_t esp_br_web_ota_status_get_handler(httpd_req_t *req);

/**
 * @brief Receive a firmware image in the request body and write it to the inactive OTA partition.
 *
 * The device reboots into the new firmware once the image has been verified.
 */
esp_err_t esp_br_web_ota_upload_post_handler(httpd_req_t *req);

/**
 * @brief Download a firmware image from a URL and write it to the inactive OTA partition.
 *
 * Body: {"url": "https://host/path/firmware.bin", "combined": false}. A combined image contains
 * both the host and the RCP firmware, as produced by the border router SDK.
 */
esp_err_t esp_br_web_ota_url_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
