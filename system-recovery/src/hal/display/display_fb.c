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

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            long int loc = (x + vinfo.xoffset) * (vinfo.bits_per_pixel / 8) +
                           (y + vinfo.yoffset) * finfo.line_length;
            fbp[loc / 4] = *color_data;
            color_data++;
        }
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
