/**
 * @file utils.c
 * @brief Utility function implementations.
 */

#include "utils.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

/* ---- Time --------------------------------------------------------------- */

static pthread_once_t tick_once = PTHREAD_ONCE_INIT;
static uint64_t       start_ms = 0;

static void tick_init(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    start_ms = ((uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec) / 1000;
}

uint32_t utils_tick_get(void)
{
    pthread_once(&tick_once, tick_init);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = ((uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec) / 1000;
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

int utils_shell_exec_timeout(const char *cmd, unsigned int timeout_sec)
{
    if (cmd == NULL) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: execute via shell */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent: wait with timeout */
    if (timeout_sec == 0) {
        /* No timeout — just wait */
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    /* Poll with timeout */
    unsigned int elapsed = 0;
    while (elapsed < timeout_sec) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (result < 0) return -1;
        sleep(1);
        elapsed++;
    }

    /* Timeout — kill the child process group */
    kill(-pid, SIGTERM);
    usleep(500000);  /* 500 ms grace period */
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
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

/* ---- MD5 (Public Domain Implementation) -------------------------------- */
/*
 * Lightweight MD5 — avoids forking md5sum on each verification.
 * Based on RFC 1321 reference implementation.
 */

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t  buffer[64];
} md5_ctx_t;

/* Constants for MD5Transform */
#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

#define MD5_F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~(z))))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~(z))))

#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac) do { \
    (a) += MD5_F((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTATE_LEFT((a),(s)); \
    (a) += (b); \
} while(0)

#define MD5_GG(a, b, c, d, x, s, ac) do { \
    (a) += MD5_G((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTATE_LEFT((a),(s)); \
    (a) += (b); \
} while(0)

#define MD5_HH(a, b, c, d, x, s, ac) do { \
    (a) += MD5_H((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTATE_LEFT((a),(s)); \
    (a) += (b); \
} while(0)

#define MD5_II(a, b, c, d, x, s, ac) do { \
    (a) += MD5_I((b),(c),(d)) + (x) + (uint32_t)(ac); \
    (a) = MD5_ROTATE_LEFT((a),(s)); \
    (a) += (b); \
} while(0)

static const uint32_t md5_PADDING[16] = {0};

static void md5_encode(uint8_t *output, const uint32_t *input, unsigned int len)
{
    for (unsigned int i = 0, j = 0; j < len; i++, j += 4) {
        output[j]     = (uint8_t)(input[i] & 0xff);
        output[j + 1] = (uint8_t)((input[i] >> 8) & 0xff);
        output[j + 2] = (uint8_t)((input[i] >> 16) & 0xff);
        output[j + 3] = (uint8_t)((input[i] >> 24) & 0xff);
    }
}

static void md5_decode(uint32_t *output, const uint8_t *input, unsigned int len)
{
    for (unsigned int i = 0, j = 0; j < len; i++, j += 4)
        output[i] = ((uint32_t)input[j]) | (((uint32_t)input[j + 1]) << 8) |
                    (((uint32_t)input[j + 2]) << 16) | (((uint32_t)input[j + 3]) << 24);
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    md5_decode(x, block, 64);

    /* Round 1 */
    MD5_FF(a, b, c, d, x[ 0], MD5_S11, 0xd76aa478);
    MD5_FF(d, a, b, c, x[ 1], MD5_S12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[ 2], MD5_S13, 0x242070db);
    MD5_FF(b, c, d, a, x[ 3], MD5_S14, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[ 4], MD5_S11, 0xf57c0faf);
    MD5_FF(d, a, b, c, x[ 5], MD5_S12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[ 6], MD5_S13, 0xa8304613);
    MD5_FF(b, c, d, a, x[ 7], MD5_S14, 0xfd469501);
    MD5_FF(a, b, c, d, x[ 8], MD5_S11, 0x698098d8);
    MD5_FF(d, a, b, c, x[ 9], MD5_S12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], MD5_S13, 0xffff5bb1);
    MD5_FF(b, c, d, a, x[11], MD5_S14, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12], MD5_S11, 0x6b901122);
    MD5_FF(d, a, b, c, x[13], MD5_S12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], MD5_S13, 0xa679438e);
    MD5_FF(b, c, d, a, x[15], MD5_S14, 0x49b40821);

    /* Round 2 */
    MD5_GG(a, b, c, d, x[ 1], MD5_S21, 0xf61e2562);
    MD5_GG(d, a, b, c, x[ 6], MD5_S22, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], MD5_S23, 0x265e5a51);
    MD5_GG(b, c, d, a, x[ 0], MD5_S24, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[ 5], MD5_S21, 0xd62f105d);
    MD5_GG(d, a, b, c, x[10], MD5_S22, 0x02441453);
    MD5_GG(c, d, a, b, x[15], MD5_S23, 0xd8a1e681);
    MD5_GG(b, c, d, a, x[ 4], MD5_S24, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[ 9], MD5_S21, 0x21e1cde6);
    MD5_GG(d, a, b, c, x[14], MD5_S22, 0xc33707d6);
    MD5_GG(c, d, a, b, x[ 3], MD5_S23, 0xf4d50d87);
    MD5_GG(b, c, d, a, x[ 8], MD5_S24, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13], MD5_S21, 0xa9e3e905);
    MD5_GG(d, a, b, c, x[ 2], MD5_S22, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[ 7], MD5_S23, 0x676f02d9);
    MD5_GG(b, c, d, a, x[12], MD5_S24, 0x8d2a4c8a);

    /* Round 3 */
    MD5_HH(a, b, c, d, x[ 5], MD5_S31, 0xfffa3942);
    MD5_HH(d, a, b, c, x[ 8], MD5_S32, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], MD5_S33, 0x6d9d6122);
    MD5_HH(b, c, d, a, x[14], MD5_S34, 0xfde5380c);
    MD5_HH(a, b, c, d, x[ 1], MD5_S31, 0xa4beea44);
    MD5_HH(d, a, b, c, x[ 4], MD5_S32, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[ 7], MD5_S33, 0xf6bb4b60);
    MD5_HH(b, c, d, a, x[10], MD5_S34, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13], MD5_S31, 0x289b7ec6);
    MD5_HH(d, a, b, c, x[ 0], MD5_S32, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[ 3], MD5_S33, 0xd4ef3085);
    MD5_HH(b, c, d, a, x[ 6], MD5_S34, 0x04881d05);
    MD5_HH(a, b, c, d, x[ 9], MD5_S31, 0xd9d4d039);
    MD5_HH(d, a, b, c, x[12], MD5_S32, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], MD5_S33, 0x1fa27cf8);
    MD5_HH(b, c, d, a, x[ 2], MD5_S34, 0xc4ac5665);

    /* Round 4 */
    MD5_II(a, b, c, d, x[ 0], MD5_S41, 0xf4292244);
    MD5_II(d, a, b, c, x[ 7], MD5_S42, 0x432aff97);
    MD5_II(c, d, a, b, x[14], MD5_S43, 0xab9423a7);
    MD5_II(b, c, d, a, x[ 5], MD5_S44, 0xfc93a039);
    MD5_II(a, b, c, d, x[12], MD5_S41, 0x655b59c3);
    MD5_II(d, a, b, c, x[ 3], MD5_S42, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], MD5_S43, 0xffeff47d);
    MD5_II(b, c, d, a, x[ 1], MD5_S44, 0x85845dd1);
    MD5_II(a, b, c, d, x[ 8], MD5_S41, 0x6fa87e4f);
    MD5_II(d, a, b, c, x[15], MD5_S42, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[ 6], MD5_S43, 0xa3014314);
    MD5_II(b, c, d, a, x[13], MD5_S44, 0x4e0811a1);
    MD5_II(a, b, c, d, x[ 4], MD5_S41, 0xf7537e82);
    MD5_II(d, a, b, c, x[11], MD5_S42, 0xbd3af235);
    MD5_II(c, d, a, b, x[ 2], MD5_S43, 0x2ad7d2bb);
    MD5_II(b, c, d, a, x[ 9], MD5_S44, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(md5_ctx_t *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *input, unsigned int input_len)
{
    unsigned int i, index, part_len;

    index = (unsigned int)((ctx->count[0] >> 3) & 0x3F);
    if ((ctx->count[0] += ((uint32_t)input_len << 3)) < ((uint32_t)input_len << 3))
        ctx->count[1]++;
    ctx->count[1] += ((uint32_t)input_len >> 29);

    part_len = 64 - index;
    if (input_len >= part_len) {
        memcpy(&ctx->buffer[index], input, part_len);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < input_len; i += 64)
            md5_transform(ctx->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ctx->buffer[index], &input[i], input_len - i);
}

static void md5_final(md5_ctx_t *ctx, uint8_t digest[16])
{
    uint8_t bits[8];
    unsigned int index, pad_len;

    md5_encode(bits, ctx->count, 8);

    index  = (unsigned int)((ctx->count[0] >> 3) & 0x3f);
    pad_len = (index < 56) ? (56 - index) : (120 - index);

    uint8_t pad[64];
    memcpy(pad, md5_PADDING, pad_len);
    pad[0] = 0x80;
    md5_update(ctx, pad, pad_len);
    md5_update(ctx, bits, 8);
    md5_encode(digest, ctx->state, 16);
}

static bool md5_file(const char *path, char hex_out[33])
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return false;

    md5_ctx_t ctx;
    md5_init(&ctx);

    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        md5_update(&ctx, buf, (unsigned int)n);

    fclose(fp);

    uint8_t digest[16];
    md5_final(&ctx, digest);

    for (int i = 0; i < 16; i++)
        sprintf(hex_out + i * 2, "%02x", digest[i]);
    hex_out[32] = '\0';

    return true;
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

    /* Compute MD5 in-process — no fork overhead */
    char actual[33] = {0};
    if (!md5_file(file_path, actual)) {
        fprintf(stderr, "utils: failed to compute MD5 for %s\n", file_path);
        return false;
    }

    bool ok = (strcmp(expected, actual) == 0);
    if (!ok) {
        fprintf(stderr, "utils: MD5 mismatch for %s (exp=%s act=%s)\n",
                file_path, expected, actual);
    }
    return ok;
}
