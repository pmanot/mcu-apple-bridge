/*
 * Log Stream Header
 * Circular buffer for log storage and SSE streaming
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the log buffer
 * Call this before any logging occurs.
 */
void log_buffer_init(void);

/**
 * @brief Add a log line to the buffer
 * Thread-safe. Called from the custom vprintf.
 *
 * @param line  Log line (will be copied)
 * @param len   Length of the line
 */
void log_buffer_add(const char *line, size_t len);

/**
 * @brief Get the next log line for a specific reader
 *
 * Each SSE client has a reader_id to track their position.
 * Returns NULL if no new logs available.
 *
 * @param reader_id  Unique ID for this reader (0-3)
 * @param out_len    Output: length of returned string
 * @return Pointer to log line (valid until next call) or NULL
 */
const char *log_buffer_read(int reader_id, size_t *out_len);

/**
 * @brief Allocate a reader ID for a new SSE client
 * @return Reader ID (0-3) or -1 if no slots available
 */
int log_buffer_alloc_reader(void);

/**
 * @brief Free a reader ID when SSE client disconnects
 */
void log_buffer_free_reader(int reader_id);

/**
 * @brief Check if there are new logs for a reader
 */
bool log_buffer_has_data(int reader_id);

/**
 * @brief Get all buffered logs as a single string
 *
 * Copies all logs in the buffer (oldest to newest) into the provided buffer.
 * Each log line is separated by a newline.
 *
 * @param out_buf    Output buffer to write logs to
 * @param buf_size   Size of output buffer
 * @return Number of bytes written (excluding null terminator)
 */
size_t log_buffer_get_all(char *out_buf, size_t buf_size);

/**
 * @brief Get the number of lines currently in buffer
 */
int log_buffer_get_count(void);

#ifdef __cplusplus
}
#endif
