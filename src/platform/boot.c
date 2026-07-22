#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"

/* Read a boot-id pseudo-file, strip trailing whitespace, demand non-empty. */
static bool read_id_trim(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t r;
    do r = read(fd, buf, n - 1); while (r < 0 && errno == EINTR);
    close(fd);
    if (r < 0) return false;
    buf[r] = '\0';
    while (r > 0 && isspace((unsigned char)buf[r - 1])) buf[--r] = '\0';
    return buf[0] != '\0';
}

bool hold_current_boot_id(char *buf, size_t n) {
    if (n == 0) return false;
    buf[0] = '\0';
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_BOOT_ID_PATH");
    if (override && *override) return read_id_trim(override, buf, n);
#endif
    if (read_id_trim(HOLD_BOOT_ID_PATH, buf, n)) return true;
#if defined(__APPLE__)
    /* No pseudo-file: synthesize from the kernel boot time — stable within a
     * boot, distinct across boots. */
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime)) {
        snprintf(buf, n, "macos-%lld.%06d", (long long)boottime.tv_sec, boottime.tv_usec);
        return true;
    }
#endif
    return false;
}

/* The common caller shape: a boot id when one is knowable, else NULL for
 * "evaluate without a boot check" (hold_eval_state and friends accept NULL). */
const char *hold_boot_id_or_null(char buf[128]) {
    return hold_current_boot_id(buf, 128) ? buf : NULL;
}
