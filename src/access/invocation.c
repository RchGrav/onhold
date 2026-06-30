#include "hold/config.h"
#include "hold/types.h"
#include "hold/access.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"

int hold_detect_invocation(struct hold_invocation *inv, bool requested_system, bool elevated) {
    memset(inv, 0, sizeof(*inv));
    inv->euid_root = (geteuid() == 0);
    inv->requested_system = requested_system;
    inv->elevated = elevated;
    inv->invoking_uid = getuid();
    inv->invoking_gid = getgid();
    snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "");
    snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", "");

    if (!inv->euid_root) {
        return 0;
    }

    const char *su = getenv("SUDO_UID");
    const char *sg = getenv("SUDO_GID");
    const char *sn = getenv("SUDO_USER");
    uid_t uid = 0;
    gid_t gid = 0;
    if (hold_parse_uid_env(su, &uid) == 0 && hold_parse_gid_env(sg, &gid) == 0 && sn && *sn) {
        struct hold_passwd_entry pw;
        if (hold_lookup_passwd_by_uid(uid, &pw) == 0 && pw.home[0]) {
            const char *home = pw.home;
#ifdef HOLD_TESTING
            const char *home_override = getenv("HOLD_TEST_INVOKING_HOME");
            if (home_override && *home_override) {
                home = home_override;
            }
#endif
            inv->have_sudo_user = true;
            inv->invoking_uid = uid;
            inv->invoking_gid = gid;
            if (hold_checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", sn) != 0 ||
                hold_checked_snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", home) != 0) {
                return -1;
            }
            return 0;
        }
    }

    inv->have_sudo_user = false;
    inv->invoking_uid = 0;
    inv->invoking_gid = 0;
    if (hold_checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "root") != 0) {
        return -1;
    }
    return 0;
}

int hold_init_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    return hold_init_user_store_from_home(inv->invoking_home, store);
}
