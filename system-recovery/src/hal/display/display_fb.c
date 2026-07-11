#include "display.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int              fbfd       = -1;
static uint32_t        *fbp        = NULL;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static long int         screensize = 0;
static bool             initialized = false;

bool display_init(void)
{
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        perror("display: cannot open /dev/fb0");
        return false;
    }

    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("display: FBIOGET_FSCREENINFO");
        close(fbfd);
        return false;
    }

    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("display: FBIOGET_VSCREENINFO");
        close(fbfd);
        return false;
    }

    screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    fbp = (uint32_t *)mmap(0, screensize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fbfd, 0);

    if ((void *)fbp == MAP_FAILED) {
        perror("display: mmap");
        close(fbfd);
        return false;
    }

    printf("display: %dx%d, %d bpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    initialized = true;
    return true;
}

void display_flush(const display_area_t *area, uint32_t *color_data)
{
    if (!initialized || fbp == NULL) return;
    if (area == NULL || color_data == NULL) return;

    /* Validate 32 bpp — this code assumes 4-byte pixels */
    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "display: unsupported bpp %d (expected 32)\n",
                vinfo.bits_per_pixel);
        return;
    }

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint32_t row_bytes = (uint32_t)w * 4;  /* 32 bpp = 4 bytes/pixel */
    uint32_t line_len  = (uint32_t)finfo.line_length;

    /* Row-by-row memcpy — far faster than per-pixel loops */
    for (int32_t y = 0; y < h; y++) {
        uint32_t y_offset = (uint32_t)(area->y1 + y + vinfo.yoffset) * line_len;
        uint32_t x_offset = (uint32_t)(area->x1 + vinfo.xoffset) * 4;
        long int byte_off = (long int)(y_offset + x_offset);

        memcpy((uint8_t *)fbp + byte_off,
               color_data + (uint32_t)y * (uint32_t)w,
               row_bytes);
    }
}

void display_wait_flush(void)
{
    /* Framebuffer writes are synchronous; nothing to wait for. */
}

void display_deinit(void)
{
    if (fbp && fbp != MAP_FAILED) {
        munmap(fbp, screensize);
        fbp = NULL;
    }
    if (fbfd >= 0) {
        close(fbfd);
        fbfd = -1;
    }
    initialized = false;
}

int display_get_width(void)  { return DISPLAY_HOR_RES; }
int display_get_height(void) { return DISPLAY_VER_RES; }
