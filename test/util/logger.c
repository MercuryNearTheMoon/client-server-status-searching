/* Tests the compile-time log macros; the logger backend is not implemented. */
#include "util/logger.h"

int main(void)
{
    SYSINFO_ERROR_LOG("error diagnostic");
    SYSINFO_WARN_LOG("warn diagnostic");
    SYSINFO_INFO_LOG("info diagnostic");
    SYSINFO_DEBUG("debug diagnostic");
    /* The runner verifies stderr for each compile-time configuration. */
    return 0;
}
