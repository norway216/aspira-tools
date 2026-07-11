#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_HOR_RES 1920
#define DISPLAY_VER_RES 1080
#define DISPLAY_BUF_SIZE (DISPLAY_HOR_RES * DISPLAY_VER_RES)

typedef struct {
    int32_t x1, y1, x2, y2;
} display_area_t;

typedef void (*display_flush_cb_t)(const display_area_t *area, uint32_t *color_data);

bool display_init(void);
void display_flush(const display_area_t *area, uint32_t *color_data);
void display_wait_flush(void);
void display_deinit(void);

int  display_get_width(void);
int  display_get_height(void);

#endif /* HAL_DISPLAY_H */
