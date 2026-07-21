#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"

static int get_boot_id(char *buf, size_t n);

static int get_boot_id(char *buf, size_t n) {
#ifdef HOLD_TESTING
    const char *path = getenv("HOLD_BOOT_ID_PATH");
    if (path && *path) {
        return hold_read_file_trim(path, buf, n);
    }
#endif
    if (hold_read_file_trim(HOLD_BOOT_ID_PATH, buf, n) == 0) {
        return 0;
    }
#if defined(__APPLE__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime)) {
        snprintf(buf, n, "macos-%lld.%06d", (long long)boottime.tv_sec, boottime.tv_usec);
        return 0;
    }
#endif
    return -1;
}

bool hold_current_boot_id(char *buf, size_t n) {
    if (n == 0) {
        return false;
    }
    buf[0] = '\0';
    return get_boot_id(buf, n) == 0 && buf[0] != '\0';
}

/* The common caller shape: a boot id when one is knowable, else NULL for
 * "evaluate without a boot check" (hold_eval_state and friends accept NULL). */
const char *hold_boot_id_or_null(char buf[128]) {
    return hold_current_boot_id(buf, 128) ? buf : NULL;
}
