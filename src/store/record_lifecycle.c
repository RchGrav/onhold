#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "record_internal.h"

/* Record lifecycle: teardown of loaded records and the exit stamp
 * (mark_run_finished), including its purged-is-final semantics. */

void hold_free_run_record(struct hold_run_record *r) {
    if (!r) return;
    hold_free_argv_alloc(r->recipe.argv, r->recipe.argc);
    hold_free_argv_alloc(r->recipe.env, r->recipe.envc);
    hold_free_argv_alloc(r->observed_argv, r->observed_argc);
    r->recipe.argv = NULL;
    r->recipe.env = NULL;
    r->observed_argv = NULL;
    r->recipe.argc = 0;
    r->recipe.envc = 0;
    r->observed_argc = 0;
}

static int wait_status_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 255;
}

int hold_mark_run_finished(const struct hold_store *store, const char *id, int status) {
    if (!store || !hold_valid_id(id)) {
        errno = EINVAL;
        return -1;
    }
    char path[HOLD_PATH_MAX];
    struct hold_run_record r;
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return -1;
    }
    struct stat old_st;
    bool have_old_st = false;
    int old_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (old_fd >= 0) {
        have_old_st = fstat(old_fd, &old_st) == 0 && S_ISREG(old_st.st_mode);
        close(old_fd);
    }

    snprintf(r.state, sizeof(r.state), "%s", "exited");
    r.has_state = true;
    r.exit_code = wait_status_exit_code(status);
    r.has_exit_code = true;
    if (WIFSIGNALED(status)) {
        r.term_signal = WTERMSIG(status);
        r.has_term_signal = true;
    } else {
        r.has_term_signal = false;
        r.term_signal = 0;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        int64_t ended_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        hold_format_rfc3339_utc_from_ns(ended_ns, r.ended_at, sizeof(r.ended_at));
        r.has_ended_at = true;
    }

    int argc = r.recipe.argc;
    char **argv = r.recipe.argv;
    if (argc <= 0 || !argv) {
        argc = r.observed_argc;
        argv = r.observed_argv;
    }
    /* A purge may have removed the record between our load and this write
     * (force-purging a live call races the reaper's exit stamp); recreating
     * it here would resurrect a call the user just removed. The re-check
     * narrows that window to the stat-to-rename gap. Return 1 — distinct
     * from -1 — so callers can tell "purged, stay gone" (terminal) from a
     * load failure such as "record not written yet" (retryable). */
    struct stat still_there;
    if (stat(path, &still_there) != 0 && errno == ENOENT) {
        return 1;
    }
    char rewritten_path[HOLD_PATH_MAX] = {0};
    int rc = hold_write_record_atomic(store->record_dir, &r, argc, argv, rewritten_path, sizeof(rewritten_path));
    if (rc == 0 && have_old_st && geteuid() == 0 && rewritten_path[0]) {
        int rewritten_fd = open(rewritten_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (rewritten_fd < 0) {
            rc = -1;
        } else {
            if (fchown(rewritten_fd, old_st.st_uid, old_st.st_gid) != 0) {
                rc = -1;
            }
            close(rewritten_fd);
        }
    }
    if (rc == 0 && store->kind == STORE_SYSTEM_MANAGED && store->public_dir[0]) {
        /* A finishing call has no live ports; the projection clears them. */
        rc = hold_write_public_index_atomic(store, &r, NULL);
    }
    hold_free_run_record(&r);
    return rc;
}
