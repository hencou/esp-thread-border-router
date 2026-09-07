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
 * @brief Receive an update image in the request body and write it to flash.
 *
 * The image is either a plain application image or an update bundle that also carries the web GUI
 * and the RCP firmware. The device reboots once the image has been verified.
 */
esp_err_t esp_br_web_ota_upload_post_handler(httpd_req_t *req);

/**
 * @brief Download an update image from a URL and write it to flash.
 *
 * Body: {"url": "https://host/path/image.bin", "combined": false}. The downloaded image may be a
 * plain application image or an update bundle. A combined image is the RCP-SDK format that carries
 * the host and the RCP firmware and is handled by esp_br_http_ota().
 */
esp_err_t esp_br_web_ota_url_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
