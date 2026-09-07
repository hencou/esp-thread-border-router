/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Remote console endpoints of the border router web server
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start capturing the device log into a ring buffer so it can be read remotely.
 *
 * The log keeps being written to the serial console as well.
 */
void esp_br_web_console_init(void);

/**
 * @brief Return the buffered device log.
 *
 * Query: ?after=<sequence> to only receive log written after a previous read. The response is
 * {"seq": <sequence of the last returned byte>, "dropped": <bytes lost>, "log": "..."}.
 */
esp_err_t esp_br_web_console_log_get_handler(httpd_req_t *req);

/**
 * @brief Run an OpenThread CLI command and return its output.
 *
 * Body: {"command": "netdata show"}. Response: {"success": true, "output": "..."}.
 */
esp_err_t esp_br_web_console_command_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
