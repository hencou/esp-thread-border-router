/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP handlers of the Wi-Fi provisioning portal.
 *
 * They are registered on the border router web server (see esp_br_web_start()) so that a single
 * HTTP server serves both the provisioning portal and the Thread web GUI, in SoftAP as well as in
 * station mode.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_br_wifi_config_scan_get_handler(httpd_req_t *req);
esp_err_t esp_br_wifi_config_submit_post_handler(httpd_req_t *req);
esp_err_t esp_br_wifi_config_status_get_handler(httpd_req_t *req);
esp_err_t esp_br_wifi_config_icon_get_handler(httpd_req_t *req);
esp_err_t esp_br_wifi_config_captive_portal_get_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
