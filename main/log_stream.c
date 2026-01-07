/*
 * Log Stream Implementation
 * Circular buffer for log storage and SSE streaming
 *
 * Design:
 * - Fixed-size circular buffer of log lines
 * - Multiple readers (SSE clients) each track their own position
 * - Thread-safe using FreeRTOS mutex
 * - Oldest logs are overwritten when buffer is full
 */

#include <string.h>
#include "log_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Configuration
#define LOG_BUFFER_LINES    100     // Number of log lines to buffer
#define LOG_LINE_MAX_LEN    256     // Max length per line
#define MAX_READERS         4       // Max concurrent SSE clients

// Circular buffer of log lines
static char s_log_buffer[LOG_BUFFER_LINES][LOG_LINE_MAX_LEN];
static size_t s_log_lengths[LOG_BUFFER_LINES];  // Actual length of each line
static int s_write_idx = 0;                      // Next write position
static int s_total_written = 0;                  // Total lines ever written (for reader sync)

// Reader tracking - each SSE client has a read position
static int s_reader_pos[MAX_READERS];    // Last read position for each reader
static bool s_reader_active[MAX_READERS]; // Is this reader slot in use?

// Thread safety
static SemaphoreHandle_t s_mutex = NULL;

void log_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    // Initialize buffer
    memset(s_log_buffer, 0, sizeof(s_log_buffer));
    memset(s_log_lengths, 0, sizeof(s_log_lengths));
    s_write_idx = 0;
    s_total_written = 0;

    // Initialize readers
    for (int i = 0; i < MAX_READERS; i++) {
        s_reader_pos[i] = 0;
        s_reader_active[i] = false;
    }
}

void log_buffer_add(const char *line, size_t len)
{
    if (!s_mutex || !line || len == 0) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;  // Skip if can't get mutex quickly (don't block logging)
    }

    // Truncate if too long
    if (len > LOG_LINE_MAX_LEN - 1) {
        len = LOG_LINE_MAX_LEN - 1;
    }

    // Copy to buffer
    memcpy(s_log_buffer[s_write_idx], line, len);
    s_log_buffer[s_write_idx][len] = '\0';
    s_log_lengths[s_write_idx] = len;

    // Advance write pointer
    s_write_idx = (s_write_idx + 1) % LOG_BUFFER_LINES;
    s_total_written++;

    xSemaphoreGive(s_mutex);
}

int log_buffer_alloc_reader(void)
{
    if (!s_mutex) return -1;

    int reader_id = -1;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_READERS; i++) {
            if (!s_reader_active[i]) {
                s_reader_active[i] = true;
                // Start reader at current position (don't replay old logs)
                s_reader_pos[i] = s_total_written;
                reader_id = i;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
    }

    return reader_id;
}

void log_buffer_free_reader(int reader_id)
{
    if (!s_mutex || reader_id < 0 || reader_id >= MAX_READERS) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_reader_active[reader_id] = false;
        xSemaphoreGive(s_mutex);
    }
}

bool log_buffer_has_data(int reader_id)
{
    if (!s_mutex || reader_id < 0 || reader_id >= MAX_READERS) return false;

    bool has_data = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_reader_active[reader_id]) {
            has_data = (s_reader_pos[reader_id] < s_total_written);
        }
        xSemaphoreGive(s_mutex);
    }

    return has_data;
}

const char *log_buffer_read(int reader_id, size_t *out_len)
{
    if (!s_mutex || reader_id < 0 || reader_id >= MAX_READERS) return NULL;
    if (!out_len) return NULL;

    const char *result = NULL;
    *out_len = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_reader_active[reader_id] && s_reader_pos[reader_id] < s_total_written) {
            // Calculate buffer index from reader position
            // Handle wrap-around: if reader is too far behind, skip to oldest available
            int lines_behind = s_total_written - s_reader_pos[reader_id];
            if (lines_behind > LOG_BUFFER_LINES) {
                // Reader fell behind, skip to oldest available
                s_reader_pos[reader_id] = s_total_written - LOG_BUFFER_LINES;
                lines_behind = LOG_BUFFER_LINES;
            }

            // Calculate actual buffer index
            int buf_idx = (s_write_idx - lines_behind + LOG_BUFFER_LINES) % LOG_BUFFER_LINES;

            result = s_log_buffer[buf_idx];
            *out_len = s_log_lengths[buf_idx];

            // Advance reader
            s_reader_pos[reader_id]++;
        }
        xSemaphoreGive(s_mutex);
    }

    return result;
}

int log_buffer_get_count(void)
{
    if (!s_mutex) return 0;

    int count = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = (s_total_written < LOG_BUFFER_LINES) ? s_total_written : LOG_BUFFER_LINES;
        xSemaphoreGive(s_mutex);
    }

    return count;
}

size_t log_buffer_get_all(char *out_buf, size_t buf_size)
{
    if (!s_mutex || !out_buf || buf_size == 0) return 0;

    size_t written = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // How many lines do we have?
        int num_lines = (s_total_written < LOG_BUFFER_LINES) ? s_total_written : LOG_BUFFER_LINES;

        if (num_lines > 0) {
            // Find the oldest line in the buffer
            int start_idx;
            if (s_total_written <= LOG_BUFFER_LINES) {
                start_idx = 0;  // Haven't wrapped yet
            } else {
                start_idx = s_write_idx;  // Oldest is at write position (about to be overwritten)
            }

            // Copy each line
            for (int i = 0; i < num_lines && written < buf_size - 2; i++) {
                int buf_idx = (start_idx + i) % LOG_BUFFER_LINES;
                size_t line_len = s_log_lengths[buf_idx];

                // Check if we have room
                if (written + line_len + 1 >= buf_size) {
                    break;  // No more room
                }

                // Copy line
                memcpy(out_buf + written, s_log_buffer[buf_idx], line_len);
                written += line_len;

                // Add newline
                out_buf[written++] = '\n';
            }
        }

        xSemaphoreGive(s_mutex);
    }

    // Null terminate
    if (written < buf_size) {
        out_buf[written] = '\0';
    } else {
        out_buf[buf_size - 1] = '\0';
    }

    return written;
}
