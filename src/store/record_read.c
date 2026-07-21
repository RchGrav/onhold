#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "record_internal.h"

/* Record loading: strict native-schema parse with checked narrowing,
 * plus id-prefix resolution to a record path. */

int hold_record_checked_i64_to_pid(int64_t v, pid_t *out) {
    pid_t narrowed = (pid_t)v;
    if ((int64_t)narrowed != v) {
        errno = ERANGE;
        return -1;
    }
    *out = narrowed;
    return 0;
}

static int checked_i64_to_uid(int64_t v, uid_t *out) {
    if (v < 0) {
        errno = ERANGE;
        return -1;
    }
    uid_t narrowed = (uid_t)v;
    if ((uintmax_t)narrowed != (uintmax_t)v) {
        errno = ERANGE;
        return -1;
    }
    *out = narrowed;
    return 0;
}

static int checked_i64_to_gid(int64_t v, gid_t *out) {
    if (v < 0) {
        errno = ERANGE;
        return -1;
    }
    gid_t narrowed = (gid_t)v;
    if ((uintmax_t)narrowed != (uintmax_t)v) {
        errno = ERANGE;
        return -1;
    }
    *out = narrowed;
    return 0;
}

int hold_load_record(const char *path, struct hold_run_record *r) {
    memset(r, 0, sizeof(*r));
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) {
        return -1;
    }

    int64_t tmp = 0;
    if (hold_json_get_i64(j, "version", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->version = (int)tmp;
    if (hold_json_get_str(j, "id", r->id, sizeof(r->id)) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_str(j, "run_id", r->run_id, sizeof(r->run_id)) != 0) {
        snprintf(r->run_id, sizeof(r->run_id), "%s", r->id);
    }
    if (hold_json_get_i64(j, "pid", &tmp) != 0 || hold_record_checked_i64_to_pid(tmp, &r->pid) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_i64(j, "pgid", &tmp) != 0 || hold_record_checked_i64_to_pid(tmp, &r->pgid) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_i64(j, "sid", &tmp) != 0 || hold_record_checked_i64_to_pid(tmp, &r->sid) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_i64(j, "start_unix_ns", &r->start_unix_ns) != 0) {
        r->start_unix_ns = 0;
    }
    if (hold_json_get_i64(j, "created_unix_ns", &r->created_unix_ns) != 0) {
        r->created_unix_ns = r->start_unix_ns;
    }
    if (hold_json_get_i64(j, "uid", &tmp) != 0) {
        free(j);
        return -1;
    }
    if (checked_i64_to_uid(tmp, &r->uid) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_i64(j, "gid", &tmp) != 0) {
        free(j);
        return -1;
    }
    if (checked_i64_to_gid(tmp, &r->gid) != 0) {
        free(j);
        return -1;
    }
    if (hold_json_get_i64(j, "invoked_by_uid", &tmp) == 0) {
        r->has_invocation = true;
        if (checked_i64_to_uid(tmp, &r->invoked_by_uid) != 0) {
            free(j);
            return -1;
        }
    }
    if (hold_json_get_i64(j, "invoked_by_gid", &tmp) == 0) {
        r->has_invocation = true;
        if (checked_i64_to_gid(tmp, &r->invoked_by_gid) != 0) {
            free(j);
            return -1;
        }
    }
    if (hold_json_get_str(j, "invoked_by_user", r->invoked_by_user, sizeof(r->invoked_by_user)) == 0) {
        r->has_invocation = true;
    }
    if (hold_json_get_bool(j, "invoked_via_sudo", &r->invoked_via_sudo) == 0) {
        r->has_invocation = true;
    }
    if (hold_json_get_str(j, "name", r->name, sizeof(r->name)) == 0 && hold_valid_alias(r->name)) {
        r->has_name = true;
    }
    if (hold_json_get_str(j, "log_path", r->log_path, sizeof(r->log_path)) == 0) {
        r->has_log = true;
    }
    if (hold_json_get_str(j, "boot_id", r->boot_id, sizeof(r->boot_id)) == 0) {
        r->has_boot = true;
    }
    if (hold_json_get_str(j, "started_at", r->started_at, sizeof(r->started_at)) == 0) {
        r->has_started_at = true;
    }
    if (hold_json_get_str(j, "created_at", r->created_at, sizeof(r->created_at)) == 0) {
        r->has_created_at = true;
    } else if (r->has_started_at) {
        snprintf(r->created_at, sizeof(r->created_at), "%s", r->started_at);
        r->has_created_at = true;
    }
    if (hold_json_get_str(j, "ended_at", r->ended_at, sizeof(r->ended_at)) == 0) {
        r->has_ended_at = true;
    }
    if (hold_json_get_str(j, "state", r->state, sizeof(r->state)) == 0) {
        r->has_state = true;
    }
    if (hold_json_get_i64(j, "exit_code", &tmp) == 0) {
        r->has_exit_code = true;
        r->exit_code = (int)tmp;
    }
    if (hold_json_get_i64(j, "term_signal", &tmp) == 0) {
        r->has_term_signal = true;
        r->term_signal = (int)tmp;
    }
    if (hold_json_get_str(j, "launch_error", r->launch_error, sizeof(r->launch_error)) == 0) {
        r->has_launch_error = true;
    }
    if (hold_json_get_str(j, "console_sock", r->console_sock, sizeof(r->console_sock)) == 0 &&
        r->console_sock[0] == '/') {
        r->has_console = true;
    }
    {
        bool saved = false;
        if (hold_json_get_bool(j, "saved", &saved) == 0) {
            r->saved = saved;
        } else if (hold_json_get_bool(j, "Saved", &saved) == 0) {
            /* Legacy shim: records written before the 0.7 single-schema cut
             * carried the saved flag only under its Docker-era spelling.
             * Remove once pre-0.7 records no longer matter. */
            r->saved = saved;
        }
    }
    const char *mode_obj = NULL;
    if (hold_json_find_key(j, "mode", &mode_obj) == 0 && mode_obj && *mode_obj == '{') {
        bool b = false;
        if (hold_json_get_bool(mode_obj, "interactive", &b) == 0) r->recipe.mode_interactive = b;
        if (hold_json_get_bool(mode_obj, "tty", &b) == 0) r->recipe.mode_tty = b;
        if (hold_json_get_bool(mode_obj, "detach", &b) == 0) r->recipe.mode_detach = b;
        if (hold_json_get_bool(mode_obj, "allow_multi", &b) == 0) r->recipe.allow_multi = b;
    }
    const char *observed = NULL;
    if (hold_json_find_key(j, "observed", &observed) == 0 && observed && *observed == '{') {
        bool have_observed = false;
        if (hold_json_get_str(observed, "exe", r->observed_exe, sizeof(r->observed_exe)) == 0) {
            have_observed = true;
        }
        if (hold_json_get_str(observed, "cwd", r->observed_cwd, sizeof(r->observed_cwd)) == 0) {
            have_observed = true;
        }
        if (hold_json_get_argv_alloc(observed, &r->observed_argv, &r->observed_argc) == 0) {
            have_observed = true;
        }
        r->has_observed = have_observed;
    }
    if (hold_json_get_argv_alloc(j, &r->recipe.argv, &r->recipe.argc) != 0) {
        r->recipe.argv = NULL;
        r->recipe.argc = 0;
    }
    if (hold_json_get_env_alloc(j, &r->recipe.env, &r->recipe.envc) != 0) {
        r->recipe.env = NULL;
        r->recipe.envc = 0;
    }
    if (hold_json_get_str(j, "restart", r->recipe.restart_policy, sizeof(r->recipe.restart_policy)) == 0 &&
        r->recipe.restart_policy[0]) {
        r->recipe.has_restart_policy = true;
    }
    if (hold_json_get_i64(j, "restart_delay_seconds", &tmp) == 0 && tmp >= 0 && tmp <= INT_MAX) {
        r->recipe.restart_delay_seconds = (int)tmp;
        r->recipe.has_restart_delay = true;
    }
    if (hold_json_get_u64(j, "proc_starttime_ticks", &r->proc_starttime_ticks) != 0 ||
        hold_json_get_u64(j, "exe_dev", &r->exe_dev) != 0 ||
        hold_json_get_u64(j, "exe_ino", &r->exe_ino) != 0) {
        free(j);
        hold_free_run_record(r);
        return -1;
    }
    if (hold_json_get_argv_display(j, r->cmdline, sizeof(r->cmdline)) != 0 &&
        hold_json_get_str(j, "cmdline_display", r->cmdline, sizeof(r->cmdline)) != 0) {
        if (r->recipe.argc <= 0 || !r->recipe.argv || hold_format_argv_human(r->cmdline, sizeof(r->cmdline), r->recipe.argc, r->recipe.argv) != 0) {
            snprintf(r->cmdline, sizeof(r->cmdline), "?");
        }
    }
    free(j);
    return 0;
}

static int resolve_record_file_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!dir || !*dir || !input || !*input || !resolved || n == 0) {
        return -1;
    }
    if (hold_valid_id(input)) {
        char exact[HOLD_PATH_MAX];
        if (hold_checked_snprintf(exact, sizeof(exact), "%s/%s.json", dir, input) == 0 &&
            access(exact, F_OK) == 0) {
            return hold_checked_snprintf(resolved, n, "%s", input);
        }
    }
    if (!hold_valid_id_prefix(input)) {
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        if (strncmp(file_id, input, strlen(input)) == 0) {
            matches++;
            if (hold_checked_snprintf(resolved, n, "%s", file_id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return matches == 1 ? 0 : -1;
}

int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n) {
    char resolved[ID_HEX_LEN + 1];
    if (resolve_record_file_id(dir, id, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    if (hold_checked_snprintf(path, n, "%s/%s.json", dir, resolved) != 0) {
        return -1;
    }
    if (hold_load_record(path, r) != 0) {
        return -1;
    }
    if (!hold_valid_record(r) || strcmp(r->id, resolved) != 0) {
        hold_free_run_record(r);
        return -1;
    }
    return 0;
}
