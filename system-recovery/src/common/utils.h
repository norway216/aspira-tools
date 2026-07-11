/**
 * @file utils.h
 * @brief General-purpose utility functions.
 */

#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Get current monotonic time in milliseconds. */
uint32_t utils_tick_get(void);

/** Sleep for a given number of milliseconds. */
void utils_sleep_ms(uint32_t ms);

/** Execute a shell command and return its exit code. */
int utils_shell_exec(const char *cmd);

/** Execute a shell command and capture its stdout (first line only). */
int utils_shell_capture(const char *cmd, char *output, size_t output_len);

/** Check if a file exists. */
bool utils_file_exists(const char *path);

/** Compute MD5 of a file and compare with expected value.
 *  @param file_path  Path to the file to check.
 *  @return true if the file matches its .md5 companion.
 */
bool utils_verify_md5(const char *file_path);

/** Create directory (recursively) if it does not exist.
 *  @return 0 on success, -1 on error.
 */
int utils_mkdir_p(const char *path);

/** Remove a file or empty directory. */
int utils_remove(const char *path);

#endif /* COMMON_UTILS_H */
