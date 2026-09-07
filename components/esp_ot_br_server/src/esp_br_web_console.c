/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Remote console: device log buffer and OpenThread CLI over HTTP
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_br_web_console.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/cli.h"

#define CONSOLE_TAG "obtr_console"

#define LOG_RING_SIZE 6144      /* bytes of device log kept for remote viewing */
#define CLI_LINE_MAX 256        /* longest CLI command and longest single output line */
#define CLI_OUTPUT_MAX 8192     /* longest captured output of a single command */
#define CLI_TIMEOUT_MS 15000    /* commands such as `ping` take a while to complete */
#define CLI_PROMPT "> "

/*-----------------------------------------------------
 Note: device log ring buffer
-----------------------------------------------------*/

static char s_log_ring[LOG_RING_SIZE];
static size_t s_log_seq = 0; /* total number of bytes ever written */
static portMUX_TYPE s_log_spinlock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_forward = NULL;

static void log_ring_append(const char *data, size_t len)
{
    if (len > LOG_RING_SIZE) {
        data += len - LOG_RING_SIZE;
        len = LOG_RING_SIZE;
    }
    portENTER_CRITICAL(&s_log_spinlock);
    size_t offset = s_log_seq % LOG_RING_SIZE;
    size_t first = LOG_RING_SIZE - offset;
    if (first > len) {
        first = len;
    }
    memcpy(s_log_ring + offset, data, first);
    if (len > first) {
        memcpy(s_log_ring, data + first, len - first);
    }
    s_log_seq += len;
    portEXIT_CRITICAL(&s_log_spinlock);
}

static int log_vprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char line[CLI_LINE_MAX];
    int len = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (len > 0) {
        log_ring_append(line, (len < (int)sizeof(line)) ? (size_t)len : sizeof(line) - 1);
    }
    return s_log_forward ? s_log_forward(format, args) : 0;
}

/*-----------------------------------------------------
 Note: OpenThread CLI ownership

 The OpenThread interpreter has a single output callback. Instead of swapping it around every
 command - which would drop the commands registered by the CLI extension - the border router owns
 it: output is mirrored to the serial console, captured for the web client when a web command is in
 flight, and the trailing prompt is used to detect the end of a command.
-----------------------------------------------------*/

static SemaphoreHandle_t s_cli_mutex = NULL; /* one command at a time */
static TaskHandle_t s_cli_waiter = NULL;     /* task waiting for the command to complete */
static char *s_cli_capture = NULL;           /* capture buffer of the web client, or NULL */
static size_t s_cli_capture_len = 0;
static bool s_cli_owned = false;

static int cli_output(void *context, const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char line[CLI_LINE_MAX];
    int len = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (len < 0) {
        return len;
    }
    if (len >= (int)sizeof(line)) {
        len = sizeof(line) - 1;
    }

    if (strcmp(line, CLI_PROMPT) == 0) {
        /* End of the command output: hand control back to the task that fed the command. */
        TaskHandle_t waiter = s_cli_waiter;
        if (waiter) {
            xTaskNotifyGive(waiter);
            return len;
        }
        return vprintf(format, args);
    }

    if (s_cli_capture && s_cli_capture_len + len < CLI_OUTPUT_MAX) {
        memcpy(s_cli_capture + s_cli_capture_len, line, len);
        s_cli_capture_len += len;
        s_cli_capture[s_cli_capture_len] = '\0';
    }
    return vprintf(format, args);
}

/** @brief Feed a line to the CLI and wait until its output has been emitted. */
static esp_err_t cli_run_line(const char *line, char *capture, size_t *capture_len)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(s_cli_owned, ESP_ERR_INVALID_STATE, CONSOLE_TAG, "CLI is not available");

    if (xSemaphoreTake(s_cli_mutex, pdMS_TO_TICKS(CLI_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_cli_capture = capture;
    s_cli_capture_len = 0;
    if (capture) {
        capture[0] = '\0';
    }
    xTaskNotifyStateClear(NULL);
    s_cli_waiter = xTaskGetCurrentTaskHandle();

    ret = esp_openthread_cli_input(line);
    if (ret == ESP_OK && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CLI_TIMEOUT_MS)) == 0) {
        ret = ESP_ERR_TIMEOUT;
    }

    s_cli_waiter = NULL;
    if (capture_len) {
        *capture_len = s_cli_capture_len;
    }
    s_cli_capture = NULL;
    xSemaphoreGive(s_cli_mutex);
    return ret;
}

/** @brief Serial console command, replacing the one that ships with esp_openthread. */
static int cli_console_command(int argc, char **argv)
{
    char line[CLI_LINE_MAX] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strncat(line, " ", sizeof(line) - strlen(line) - 1);
        }
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }
    if (strlen(line) == 0) {
        printf("Invalid OpenThread command\n");
        return 0;
    }

    if (cli_run_line(line, NULL, NULL) != ESP_OK) {
        printf("Failed to run command: %s\n", line);
    }
    return 0;
}

esp_err_t esp_br_web_console_take_over_cli(void)
{
    ESP_RETURN_ON_FALSE(esp_openthread_get_instance() != NULL, ESP_ERR_INVALID_STATE, CONSOLE_TAG,
                        "OpenThread is not running");

    if (!s_cli_mutex) {
        s_cli_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_cli_mutex, ESP_ERR_NO_MEM, CONSOLE_TAG, "Failed to allocate CLI mutex");
    }

    /* The interpreter is recreated here, so this has to run before the CLI extension and any other
       component registers its commands. */
    esp_openthread_cli_console_command_unregister();
    esp_openthread_lock_acquire(portMAX_DELAY);
    otCliInit(esp_openthread_get_instance(), cli_output, NULL);
    esp_openthread_lock_release();
    s_cli_owned = true;

    esp_console_cmd_t cmd = {
        .command = CONFIG_OPENTHREAD_CONSOLE_COMMAND_PREFIX,
        .help = "Execute `" CONFIG_OPENTHREAD_CONSOLE_COMMAND_PREFIX " ...` to run an OpenThread CLI command",
        .hint = NULL,
        .func = cli_console_command,
    };
    return esp_console_cmd_register(&cmd);
}

/*-----------------------------------------------------
 Note: HTTP handlers
-----------------------------------------------------*/

void esp_br_web_console_init(void)
{
    if (!s_log_forward) {
        s_log_forward = esp_log_set_vprintf(log_vprintf);
    }
}

esp_err_t esp_br_web_console_log_get_handler(httpd_req_t *req)
{
    size_t after = 0;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0 && query_len < 64) {
        char query[64];
        char value[24];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "after", value, sizeof(value)) == ESP_OK) {
            after = (size_t)strtoul(value, NULL, 10);
        }
    }

    char *chunk = malloc(LOG_RING_SIZE + 1);
    ESP_RETURN_ON_FALSE(chunk, ESP_ERR_NO_MEM, CONSOLE_TAG, "Failed to allocate log buffer");

    portENTER_CRITICAL(&s_log_spinlock);
    size_t seq = s_log_seq;
    size_t available = (seq < LOG_RING_SIZE) ? seq : LOG_RING_SIZE;
    size_t oldest = seq - available;
    size_t start = (after > oldest) ? after : oldest;
    if (start > seq) {
        start = seq; /* the client asked for log that does not exist yet, e.g. after a reboot */
    }
    size_t len = seq - start;
    for (size_t i = 0; i < len; i++) {
        chunk[i] = s_log_ring[(start + i) % LOG_RING_SIZE];
    }
    portEXIT_CRITICAL(&s_log_spinlock);
    chunk[len] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddNumberToObject(root, "dropped", (after < oldest) ? (oldest - after) : 0);
    cJSON_AddStringToObject(root, "log", chunk);
    free(chunk);

    char *packet = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(packet, ESP_ERR_NO_MEM, CONSOLE_TAG, "Failed to serialize log");

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, packet);
    cJSON_free(packet);
    return ret;
}

esp_err_t esp_br_web_console_command_post_handler(httpd_req_t *req)
{
    char body[CLI_LINE_MAX + 64];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command is too long");
        return ESP_OK;
    }

    int received = 0;
    while (received < req->content_len) {
        int len = httpd_req_recv(req, body + received, req->content_len - received);
        if (len <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read the request");
            return ESP_FAIL;
        }
        received += len;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    cJSON *command_item = json ? cJSON_GetObjectItemCaseSensitive(json, "command") : NULL;
    if (!cJSON_IsString(command_item) || strlen(command_item->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"output\":\"Missing command\"}");
        return ESP_OK;
    }

    char line[CLI_LINE_MAX];
    strlcpy(line, command_item->valuestring, sizeof(line));
    cJSON_Delete(json);

    char *output = malloc(CLI_OUTPUT_MAX);
    ESP_RETURN_ON_FALSE(output, ESP_ERR_NO_MEM, CONSOLE_TAG, "Failed to allocate output buffer");

    size_t output_len = 0;
    esp_err_t err = cli_run_line(line, output, &output_len);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "output", output);
        cJSON_AddBoolToObject(root, "truncated", output_len + CLI_LINE_MAX >= CLI_OUTPUT_MAX);
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddStringToObject(root, "output", "The OpenThread CLI is not running yet");
    } else if (err == ESP_ERR_TIMEOUT) {
        cJSON_AddStringToObject(root, "output", "The command did not complete in time");
    } else {
        cJSON_AddStringToObject(root, "output", esp_err_to_name(err));
    }
    free(output);

    char *packet = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(packet, ESP_ERR_NO_MEM, CONSOLE_TAG, "Failed to serialize the output");

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, packet);
    cJSON_free(packet);
    return ret;
}
