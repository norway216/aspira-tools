#ifndef UI_SCREEN_PROGRESS_H
#define UI_SCREEN_PROGRESS_H

#include "ui/ui_manager.h"

const screen_interface_t *screen_progress_get_interface(void);

/** Apply deferred LVGL progress updates. Must be called from main thread. */
void screen_progress_apply_updates(void);

#endif /* UI_SCREEN_PROGRESS_H */
