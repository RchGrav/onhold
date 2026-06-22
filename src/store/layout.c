#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/store.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"

static int chown_if_root(const char *path, uid_t uid, gid_t gid);
static int init_user_store_for_current_user(struct store_paths *store);
static int ensure_user_store_from_home_owned(const char *home, uid_t uid, gid_t gid, struct store_paths *store);
static bool id_collides_in_store(const struct store_paths *store, const char *id, bool include_public);
static bool id_material_collides_in_store(const struct store_paths *store, const char *id, bool include_public);

int chown_root_if_root(const char *path) {
    if (geteuid() != 0) {
        return 0;
    }
    if (chown(path, 0, 0) != 0) {
        return -1;
    }
    return 0;
}

static int chown_if_root(const char *path, uid_t uid, gid_t gid) {
    if (geteuid() != 0) {
        return 0;
    }
    if (chown(path, uid, gid) != 0) {
        return -1;
    }
    return 0;
}

int init_user_store_from_home(const char *home, struct store_paths *store) {
    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }
    memset(store, 0, sizeof(*store));
    store->kind = STORE_USER_LOCAL;
    if (checked_snprintf(store->base, sizeof(store->base), "%s/.local/state/sigmund", home) != 0 ||
        checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s", store->base) != 0 ||
        checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s", store->base) != 0 ||
        checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", store->base) != 0 ||
        checked_snprintf(store->profile_path, sizeof(store->profile_path), "%s/profiles.json", store->base) != 0 ||
        checked_snprintf(store->alias_path, sizeof(store->alias_path), "%s/aliases.json", store->base) != 0) {
        return -1;
    }
    return 0;
}

static int init_user_store_for_current_user(struct store_paths *store) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "sigmund: error: HOME is not set\n");
        errno = EINVAL;
        return -1;
    }
    return init_user_store_from_home(home, store);
}

int ensure_user_store_for_current_user(struct store_paths *store) {
    if (init_user_store_for_current_user(store) != 0) {
        return -1;
    }
    if (mkdir_p0700(store->base) != 0) {
        return -1;
    }
    if (chmod(store->base, 0700) != 0) {
        return -1;
    }
    if (mkdir_p0700(store->console_dir) != 0 ||
        chmod(store->console_dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

static int ensure_user_store_from_home_owned(const char *home, uid_t uid, gid_t gid, struct store_paths *store) {
    char local_dir[SIGMUND_PATH_MAX], state_dir[SIGMUND_PATH_MAX];
    if (init_user_store_from_home(home, store) != 0) {
        return -1;
    }
    if (checked_snprintf(local_dir, sizeof(local_dir), "%s/.local", home) != 0 ||
        checked_snprintf(state_dir, sizeof(state_dir), "%s/.local/state", home) != 0) {
        return -1;
    }
    if (mkdir_p0700(store->base) != 0 ||
        chmod(local_dir, 0700) != 0 ||
        chown_if_root(local_dir, uid, gid) != 0 ||
        chmod(state_dir, 0700) != 0 ||
        chown_if_root(state_dir, uid, gid) != 0 ||
        chmod(store->base, 0700) != 0 ||
        chown_if_root(store->base, uid, gid) != 0) {
        return -1;
    }
    if (mkdir_p0700(store->console_dir) != 0 ||
        chmod(store->console_dir, 0700) != 0 ||
        chown_if_root(store->console_dir, uid, gid) != 0) {
        return -1;
    }
    return 0;
}

int ensure_invoking_user_store(const struct invocation *inv, struct store_paths *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    return ensure_user_store_from_home_owned(inv->invoking_home, inv->invoking_uid, inv->invoking_gid, store);
}

int init_system_store(struct store_paths *store) {
    const char *base = SIGMUND_SYSTEM_STATE_DIR;
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_SYSTEM_STATE_DIR");
    if (override && *override) {
        base = override;
    }
#endif
    memset(store, 0, sizeof(*store));
    store->kind = STORE_SYSTEM_MANAGED;
    if (checked_snprintf(store->base, sizeof(store->base), "%s", base) != 0 ||
        checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s/runs", base) != 0 ||
        checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s/logs", base) != 0 ||
        checked_snprintf(store->public_dir, sizeof(store->public_dir), "%s/public", base) != 0 ||
        checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", base) != 0 ||
        checked_snprintf(store->profile_path, sizeof(store->profile_path), "%s/profiles.json", base) != 0 ||
        checked_snprintf(store->alias_path, sizeof(store->alias_path), "%s/public/aliases.json", base) != 0) {
        return -1;
    }
    return 0;
}

int ensure_system_store(struct store_paths *store) {
    if (init_system_store(store) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->base, 0755) != 0 ||
        chmod(store->base, 0755) != 0 ||
        chown_root_if_root(store->base) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->record_dir, 0700) != 0 ||
        chmod(store->record_dir, 0700) != 0 ||
        chown_root_if_root(store->record_dir) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->log_dir, 0700) != 0 ||
        chmod(store->log_dir, 0700) != 0 ||
        chown_root_if_root(store->log_dir) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->console_dir, 0700) != 0 ||
        chmod(store->console_dir, 0700) != 0 ||
        chown_root_if_root(store->console_dir) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->public_dir, 0755) != 0 ||
        chmod(store->public_dir, 0755) != 0 ||
        chown_root_if_root(store->public_dir) != 0) {
        return -1;
    }
    return 0;
}

static bool id_collides_in_store(const struct store_paths *store, const char *id, bool include_public) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (store->console_dir[0] &&
        checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (include_public && store->public_dir[0]) {
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && path_exists(path)) {
            return true;
        }
    }
    return false;
}

static bool id_material_collides_in_store(const struct store_paths *store, const char *id, bool include_public) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (include_public && store->public_dir[0]) {
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && path_exists(path)) {
            return true;
        }
    }
    if (store->console_dir[0] &&
        checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    return false;
}

int gen_id_for_store(const struct store_paths *primary,
                            const struct store_paths *avoid_public_store,
                            const struct store_paths *avoid_user_store,
                            char *out,
                            size_t out_n) {
    if (out_n < ID_HEX_LEN + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    uint8_t b[ID_HEX_LEN / 2];
    char reserve[SIGMUND_PATH_MAX];
    for (int tries = 0; tries < 100; tries++) {
        if (rand_bytes(b, sizeof(b)) != 0) {
            return -1;
        }
        for (size_t i = 0; i < sizeof(b); i++) {
            snprintf(out + i * 2, out_n - i * 2, "%02x", b[i]);
        }
        out[ID_HEX_LEN] = '\0';
        if (!valid_id(out)) {
            continue;
        }
        if (id_collides_in_store(primary, out, primary->kind == STORE_SYSTEM_MANAGED)) {
            continue;
        }
        if (avoid_public_store && avoid_public_store->public_dir[0]) {
            char p[SIGMUND_PATH_MAX];
            if (checked_snprintf(p, sizeof(p), "%s/%s.json", avoid_public_store->public_dir, out) == 0 && path_exists(p)) {
                continue;
            }
        }
        if (avoid_user_store && id_collides_in_store(avoid_user_store, out, false)) {
            continue;
        }
        if (checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", primary->record_dir, out) != 0) {
            return -1;
        }
        int fd = open(reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            bool avoid_public_race = false;
            if (avoid_public_store && avoid_public_store->public_dir[0]) {
                char p[SIGMUND_PATH_MAX];
                avoid_public_race = (checked_snprintf(p, sizeof(p), "%s/%s.json", avoid_public_store->public_dir, out) == 0 && path_exists(p));
            }
            if (id_material_collides_in_store(primary, out, primary->kind == STORE_SYSTEM_MANAGED) ||
                avoid_public_race ||
                (avoid_user_store && id_material_collides_in_store(avoid_user_store, out, false))) {
                unlink(reserve);
                continue;
            }
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}
