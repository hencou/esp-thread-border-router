/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Streaming writer for update images: either a plain application image or an OTBR update bundle
 * that also carries the web GUI and the RCP firmware.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read up to @p len bytes of the update image.
 *
 * @return the number of bytes read, 0 at the end of the stream, a negative value on failure.
 */
typedef int (*esp_br_ota_read_fn)(void *ctx, char *buf, size_t len);

/**
 * @brief Write an update image that is read from @p read_fn to flash.
 *
 * The image is either an OTBR update bundle (see tools/make_ota_bundle.py) or a plain application
 * image; the format is detected from the first bytes of the stream. A bundle updates every
 * partition it contains, so the web GUI and the RCP firmware come along with the application.
 *
 * @param[in] prefetched     bytes of the stream that have already been read, may be NULL
 * @param[in] prefetched_len number of bytes in @p prefetched
 * @param[in] read_fn        reader for the remainder of the stream
 * @param[in] ctx            context passed to @p read_fn
 * @param[in] total_size     size of the whole image, 0 when it is not known upfront
 * @param[out] written       running number of payload bytes written, may be NULL
 * @param[out] total         number of payload bytes to write, may be NULL
 */
esp_err_t esp_br_ota_apply_stream(const void *prefetched, size_t prefetched_len, esp_br_ota_read_fn read_fn, void *ctx,
                                  size_t total_size, size_t *written, size_t *total);

/**
 * @brief Number of leading bytes esp_br_ota_apply_stream() needs to detect the image format.
 */
size_t esp_br_ota_detect_len(void);

#ifdef __cplusplus
}
#endif
