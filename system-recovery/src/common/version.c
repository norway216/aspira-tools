#include "version.h"

const char *get_build_id(void)
{
    return SYSTEM_RECOVERY_BUILD_ID;
}

const char *get_version(void)
{
    return SYSTEM_RECOVERY_VERSION;
}
