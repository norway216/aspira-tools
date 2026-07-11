#include "op_interface.h"
#include <stdio.h>
#include <string.h>

#define MAX_PLUGINS 32

static const operation_plugin_t *plugins[MAX_PLUGINS];
static int                       plugin_count = 0;

int operation_plugin_register(const operation_plugin_t *plugin)
{
    if (plugin == NULL || plugin->name == NULL) return -1;

    if (plugin_count >= MAX_PLUGINS) {
        fprintf(stderr, "op: plugin table full\n");
        return -1;
    }

    plugins[plugin_count++] = plugin;
    printf("op: registered plugin '%s' (%s)\n", plugin->name, plugin->description);
    return 0;
}

const operation_plugin_t *operation_plugin_find(const char *name)
{
    for (int i = 0; i < plugin_count; i++) {
        if (strcmp(plugins[i]->name, name) == 0)
            return plugins[i];
    }
    return NULL;
}

void operation_plugin_deregister_all(void)
{
    plugin_count = 0;
}
