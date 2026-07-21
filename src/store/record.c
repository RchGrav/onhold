#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "record_internal.h"

/* The private call record writer: one native snake_case schema, written
 * atomically (temp + fsync + rename) and never partially visible. */

/* Emit one optional string field: nothing when absent. */
static void write_str_field(FILE *f, const char *key, const char *val, bool present) {
    if (!present) return;
    fprintf(f, "  \"%s\": \"", key);
    hold_json_escape(f, val);
    fprintf(f, "\",\n");
}

int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n) {
    char tmp[HOLD_PATH_MAX], fin[HOLD_PATH_MAX], reserve[HOLD_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (hold_checked_snprintf(fin, sizeof(fin), "%s/%s.json", dir, r->id) != 0) {
        return -1;
    }
    if (hold_checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, r->id) != 0) {
        return -1;
    }
    if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        fd = -1;
        goto out;
    }

    /* One schema: the native snake_case keys below are the record format.
     * (The Docker-shaped parallel projection was removed in 0.7; Docker
     * parity lives at the CLI surface — flags and tables — not in storage.) */
    fprintf(f, "{\n");
    fprintf(f, "  \"saved\": %s,\n", r->saved ? "true" : "false");
    fprintf(f, "  \"version\": %d,\n", r->version);
    fprintf(f, "  \"id\": \"");
    hold_json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"run_id\": \"");
    hold_json_escape(f, r->run_id[0] ? r->run_id : r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"pid\": %ld,\n", (long)r->pid);
    fprintf(f, "  \"pgid\": %ld,\n", (long)r->pgid);
    fprintf(f, "  \"sid\": %ld,\n", (long)r->sid);
    fprintf(f, "  \"start_unix_ns\": %" PRId64 ",\n", r->start_unix_ns);
    fprintf(f, "  \"created_unix_ns\": %" PRId64 ",\n", r->created_unix_ns ? r->created_unix_ns : r->start_unix_ns);
    fprintf(f, "  \"argv\": ");
    hold_write_json_argv(f, argc, argv);
    fprintf(f, ",\n");
    if (r->has_observed) {
        fprintf(f, "  \"observed\": {\n");
        fprintf(f, "    \"exe\": \"");
        hold_json_escape(f, r->observed_exe);
        fprintf(f, "\",\n");
        fprintf(f, "    \"argv\": ");
        hold_write_json_argv(f, r->observed_argc, r->observed_argv);
        fprintf(f, ",\n");
        fprintf(f, "    \"cwd\": \"");
        hold_json_escape(f, r->observed_cwd);
        fprintf(f, "\"\n");
        fprintf(f, "  },\n");
    }
    fprintf(f, "  \"normalized\": {\n");
    fprintf(f, "    \"argv\": ");
    hold_write_json_argv(f, argc, argv);
    fprintf(f, "\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"cmdline_display\": \"");
    hold_json_escape(f, r->cmdline);
    fprintf(f, "\",\n");
    write_str_field(f, "name", r->name, r->has_name);
    write_str_field(f, "started_at", r->started_at, r->has_started_at);
    write_str_field(f, "created_at", r->created_at, r->has_created_at);
    write_str_field(f, "ended_at", r->ended_at, r->has_ended_at);
    write_str_field(f, "state", r->state, r->has_state);
    if (r->has_exit_code) {
        fprintf(f, "  \"exit_code\": %d,\n", r->exit_code);
    }
    if (r->has_term_signal) {
        fprintf(f, "  \"term_signal\": %d,\n", r->term_signal);
    }
    write_str_field(f, "launch_error", r->launch_error, r->has_launch_error);
    write_str_field(f, "console_sock", r->console_sock, r->has_console);
    if (r->recipe.envc > 0 && r->recipe.env) {
        fprintf(f, "  \"env\": ");
        hold_write_json_argv(f, r->recipe.envc, r->recipe.env);
        fprintf(f, ",\n");
    }
    if (r->recipe.mode_interactive || r->recipe.mode_tty || r->recipe.mode_detach || r->recipe.allow_multi) {
        fprintf(f, "  \"mode\": {");
        bool wrote_mode = false;
        if (r->recipe.mode_interactive) { fprintf(f, "%s\"interactive\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->recipe.mode_tty) { fprintf(f, "%s\"tty\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->recipe.mode_detach) { fprintf(f, "%s\"detach\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->recipe.allow_multi) { fprintf(f, "%s\"allow_multi\": true", wrote_mode ? ", " : ""); }
        fprintf(f, "},\n");
    }
    if (r->recipe.has_restart_policy && r->recipe.restart_policy[0]) {
        fprintf(f, "  \"restart\": \"");
        hold_json_escape(f, r->recipe.restart_policy);
        fprintf(f, "\",\n");
    }
    if (r->recipe.has_restart_delay) {
        fprintf(f, "  \"restart_delay_seconds\": %d,\n", r->recipe.restart_delay_seconds);
    }
    fprintf(f, "  \"uid\": %u,\n", r->uid);
    fprintf(f, "  \"gid\": %u,\n", r->gid);
    if (r->has_invocation) {
        fprintf(f, "  \"invoked_by_uid\": %u,\n", r->invoked_by_uid);
        fprintf(f, "  \"invoked_by_gid\": %u,\n", r->invoked_by_gid);
        fprintf(f, "  \"invoked_by_user\": \"");
        hold_json_escape(f, r->invoked_by_user);
        fprintf(f, "\",\n");
        fprintf(f, "  \"invoked_via_sudo\": %s,\n", r->invoked_via_sudo ? "true" : "false");
    }
    if (r->has_log) {
        fprintf(f, "  \"log_path\": \"");
        hold_json_escape(f, r->log_path);
        fprintf(f, "\",\n");
        char idx_path[HOLD_PATH_MAX];
        if (hold_log_idx_path(r->log_path, idx_path, sizeof(idx_path)) == 0) {
            fprintf(f, "  \"log_idx_path\": \"");
            hold_json_escape(f, idx_path);
            fprintf(f, "\",\n");
        }
    }
    write_str_field(f, "boot_id", r->boot_id, r->has_boot);
    fprintf(f, "  \"proc_starttime_ticks\": %" PRIu64 ",\n", r->proc_starttime_ticks);
    fprintf(f, "  \"exe_dev\": %" PRIu64 ",\n", r->exe_dev);
    fprintf(f, "  \"exe_ino\": %" PRIu64 "\n", r->exe_ino);
    fprintf(f, "}\n");

    if (ferror(f) || fflush(f) != 0) {
        goto out;
    }
    if (fsync(fd) != 0) {
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        goto out;
    }
    f = NULL;
    fd = -1;
    if (rename(tmp, fin) != 0) {
        goto out;
    }
    unlink(reserve);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0) {
            fprintf(stderr, "hold: warning: failed to fsync storage dir: %s\n", strerror(errno));
        }
        close(dfd);
    }
    if (out_json_path && hold_checked_snprintf(out_json_path, out_n, "%s", fin) != 0) {
        goto out;
    }
    rc = 0;

out:
    if (f) {
        fclose(f);
    } else if (fd >= 0) {
        close(fd);
    }
    if (rc != 0) {
        unlink(tmp);
    }
    return rc;
}
