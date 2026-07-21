#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"

static int chown_if_root(const char *path, uid_t uid, gid_t gid);
static int init_user_store_for_current_user(struct hold_store *store);
static int ensure_user_store_from_home_owned(const char *home, uid_t uid, gid_t gid, struct hold_store *store);

static int chown_if_root(const char *path, uid_t uid, gid_t gid) {
    return hold_chown_dir_no_symlink_if_root(path, uid, gid);
}

int hold_init_user_store_from_home(const char *home, struct hold_store *store) {
    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }
    char resolved_home[HOLD_PATH_MAX];
    const char *base_home = home;
    if (realpath(home, resolved_home)) {
        base_home = resolved_home;
    }
    memset(store, 0, sizeof(*store));
    store->kind = STORE_USER_LOCAL;
    if (hold_checked_snprintf(store->base, sizeof(store->base), "%s/.local/state/hold", base_home) != 0 ||
        hold_checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s", store->base) != 0 ||
        hold_checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s", store->base) != 0 ||
        hold_checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", store->base) != 0) {
        return -1;
    }
    return 0;
}

static int init_user_store_for_current_user(struct hold_store *store) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "hold: error: HOME is not set\n");
        errno = EINVAL;
        return -1;
    }
    return hold_init_user_store_from_home(home, store);
}

int hold_ensure_user_store_for_current_user(struct hold_store *store) {
    if (init_user_store_for_current_user(store) != 0) {
        return -1;
    }
    if (hold_mkdir_p0700(store->base) != 0) {
        return -1;
    }
    if (hold_chmod_dir_no_symlink(store->base, 0700) != 0) {
        return -1;
    }
    if (hold_mkdir_p0700(store->console_dir) != 0 ||
        hold_chmod_dir_no_symlink(store->console_dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

static int ensure_user_store_from_home_owned(const char *home, uid_t uid, gid_t gid, struct hold_store *store) {
    char local_dir[HOLD_PATH_MAX], state_dir[HOLD_PATH_MAX];
    if (hold_init_user_store_from_home(home, store) != 0) {
        return -1;
    }
    if (hold_checked_snprintf(local_dir, sizeof(local_dir), "%s/.local", home) != 0 ||
        hold_checked_snprintf(state_dir, sizeof(state_dir), "%s/.local/state", home) != 0) {
        return -1;
    }
    if (hold_mkdir_p0700(store->base) != 0 ||
        hold_chmod_dir_no_symlink(local_dir, 0700) != 0 ||
        chown_if_root(local_dir, uid, gid) != 0 ||
        hold_chmod_dir_no_symlink(state_dir, 0700) != 0 ||
        chown_if_root(state_dir, uid, gid) != 0 ||
        hold_chmod_dir_no_symlink(store->base, 0700) != 0 ||
        chown_if_root(store->base, uid, gid) != 0) {
        return -1;
    }
    if (hold_mkdir_p0700(store->console_dir) != 0 ||
        hold_chmod_dir_no_symlink(store->console_dir, 0700) != 0 ||
        chown_if_root(store->console_dir, uid, gid) != 0) {
        return -1;
    }
    return 0;
}

int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    return ensure_user_store_from_home_owned(inv->invoking_home, inv->invoking_uid, inv->invoking_gid, store);
}

int hold_init_system_store(struct hold_store *store) {
    const char *base = HOLD_SYSTEM_STATE_DIR;
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_SYSTEM_STATE_DIR");
    if (override && *override) {
        base = override;
    }
#endif
    memset(store, 0, sizeof(*store));
    store->kind = STORE_SYSTEM_MANAGED;
    if (hold_checked_snprintf(store->base, sizeof(store->base), "%s", base) != 0 ||
        hold_checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s/runs", base) != 0 ||
        hold_checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s/logs", base) != 0 ||
        hold_checked_snprintf(store->public_dir, sizeof(store->public_dir), "%s/public", base) != 0 ||
        hold_checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", base) != 0) {
        return -1;
    }
    return 0;
}

int hold_ensure_system_store(struct hold_store *store) {
    if (hold_init_system_store(store) != 0) {
        return -1;
    }
    if (hold_mkdir_p_mode(store->base, 0755) != 0 ||
        hold_chmod_dir_no_symlink(store->base, 0755) != 0 ||
        hold_chown_dir_no_symlink_if_root(store->base, 0, 0) != 0) {
        return -1;
    }
    if (hold_mkdir_p_mode(store->record_dir, 0700) != 0 ||
        hold_chmod_dir_no_symlink(store->record_dir, 0700) != 0 ||
        hold_chown_dir_no_symlink_if_root(store->record_dir, 0, 0) != 0) {
        return -1;
    }
    if (hold_mkdir_p_mode(store->log_dir, 0700) != 0 ||
        hold_chmod_dir_no_symlink(store->log_dir, 0700) != 0 ||
        hold_chown_dir_no_symlink_if_root(store->log_dir, 0, 0) != 0) {
        return -1;
    }
    if (hold_mkdir_p_mode(store->console_dir, 0700) != 0 ||
        hold_chmod_dir_no_symlink(store->console_dir, 0700) != 0 ||
        hold_chown_dir_no_symlink_if_root(store->console_dir, 0, 0) != 0) {
        return -1;
    }
    if (hold_mkdir_p_mode(store->public_dir, 0755) != 0 ||
        hold_chmod_dir_no_symlink(store->public_dir, 0755) != 0 ||
        hold_chown_dir_no_symlink_if_root(store->public_dir, 0, 0) != 0) {
        return -1;
    }
    return 0;
}

