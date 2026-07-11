/**
 * @file utils.c
 * @brief Utility function implementations.
 */

#include "utils.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

/* ---- Time --------------------------------------------------------------- */

uint32_t utils_tick_get(void)
{
    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        start_ms = (tv.tv_sec * 1000000ULL + tv.tv_usec) / 1000;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (tv.tv_sec * 1000000ULL + tv.tv_usec) / 1000;
    return (uint32_t)(now_ms - start_ms);
}

/* LVGL custom tick source (required by LV_TICK_CUSTOM) */
uint32_t custom_tick_get(void)
{
    return utils_tick_get();
}

void utils_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000);
}

/* ---- Shell ------------------------------------------------------------- */

int utils_shell_exec(const char *cmd)
{
    if (cmd == NULL) return -1;
    return system(cmd);
}

int utils_shell_capture(const char *cmd, char *output, size_t output_len)
{
    if (cmd == NULL || output == NULL || output_len == 0) return -1;

    FILE *fp = popen(cmd, "r");
    if (fp == NULL) return -1;

    if (fgets(output, (int)output_len, fp) == NULL) {
        pclose(fp);
        return -1;
    }

    /* Strip trailing newline */
    size_t len = strlen(output);
    if (len > 0 && output[len - 1] == '\n') {
        output[len - 1] = '\0';
    }

    int ret = pclose(fp);
    return (ret == 0) ? 0 : -1;
}

/* ---- Filesystem --------------------------------------------------------- */

bool utils_file_exists(const char *path)
{
    if (path == NULL) return false;
    return access(path, F_OK) == 0;
}

int utils_mkdir_p(const char *path)
{
    if (path == NULL) return -1;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int utils_remove(const char *path)
{
    if (path == NULL) return -1;
    return remove(path);
}

/* ---- MD5 Verification -------------------------------------------------- */

bool utils_verify_md5(const char *file_path)
{
    if (file_path == NULL) return false;

    /* Build path to .md5 companion file */
    char md5_path[512];
    snprintf(md5_path, sizeof(md5_path), "%s.md5", file_path);

    /* Read expected hash */
    FILE *fp = fopen(md5_path, "r");
    if (fp == NULL) {
        fprintf(stderr, "utils: cannot open MD5 file: %s\n", md5_path);
        return false;
    }

    char expected[64] = {0};
    if (fgets(expected, sizeof(expected), fp) == NULL) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    /* Strip trailing newline / whitespace */
    char *nl = strchr(expected, '\n');
    if (nl) *nl = '\0';
    nl = strchr(expected, ' ');
    if (nl) *nl = '\0';

    /* Compute actual MD5 */
    char cmd[512];
    char actual[64] = {0};
    snprintf(cmd, sizeof(cmd), "md5sum %s | cut -d' ' -f1", file_path);

    fp = popen(cmd, "r");
    if (fp == NULL) {
        fprintf(stderr, "utils: failed to compute MD5 for %s\n", file_path);
        return false;
    }

    if (fgets(actual, sizeof(actual), fp) == NULL) {
        pclose(fp);
        return false;
    }
    pclose(fp);

    nl = strchr(actual, '\n');
    if (nl) *nl = '\0';

    bool ok = (strcmp(expected, actual) == 0);
    if (!ok) {
        fprintf(stderr, "utils: MD5 mismatch for %s (exp=%s act=%s)\n",
                file_path, expected, actual);
    }
    return ok;
}
