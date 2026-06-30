#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"


void hold_free_run_record(struct hold_run_record *r) {
    if (!r) return;
    hold_free_argv_alloc(r->argv, r->argc);
    hold_free_argv_alloc(r->env, r->envc);
    hold_free_argv_alloc(r->ports, r->portc);
    hold_free_argv_alloc(r->volumes, r->volumec);
    hold_free_argv_alloc(r->cap_add, r->cap_addc);
    hold_free_argv_alloc(r->cap_drop, r->cap_dropc);
    hold_free_argv_alloc(r->observed_argv, r->observed_argc);
    r->argv = NULL;
    r->env = NULL;
    r->ports = NULL;
    r->volumes = NULL;
    r->cap_add = NULL;
    r->cap_drop = NULL;
    r->observed_argv = NULL;
    r->argc = 0;
    r->envc = 0;
    r->portc = 0;
    r->volumec = 0;
    r->cap_addc = 0;
    r->cap_dropc = 0;
    r->observed_argc = 0;
}

static void write_json_argv_tail(FILE *f, int argc, char **argv) {
    fputc('[', f);
    for (int i = 1; i < argc; i++) {
        if (i > 1) fputs(", ", f);
        fputc('"', f);
        hold_json_escape(f, argv[i] ? argv[i] : "");
        fputc('"', f);
    }
    fputc(']', f);
}

static const char *record_status(const struct hold_run_record *r) {
    if (r && r->has_state && r->state[0]) return r->state;
    return "unknown";
}

static bool record_running(const struct hold_run_record *r) {
    return r && r->has_state && strcmp(r->state, "running") == 0;
}

static const char *record_created_timestamp(const struct hold_run_record *r, const char *fallback) {
    if (r && r->has_created_at && r->created_at[0]) return r->created_at;
    if (r && r->has_started_at && r->started_at[0]) return r->started_at;
    return fallback ? fallback : "0001-01-01T00:00:00Z";
}

static void write_docker_identity_fields(FILE *f, const struct hold_run_record *r, const char *created) {
    fprintf(f, "  \"Id\": \"");
    hold_json_escape(f, r ? r->id : "");
    fprintf(f, "\",\n");
    fprintf(f, "  \"Created\": \"");
    hold_json_escape(f, created ? created : record_created_timestamp(r, "0001-01-01T00:00:00Z"));
    fprintf(f, "\",\n");
    if (r && r->has_alias) {
        fprintf(f, "  \"Origin\": \"");
        hold_json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
}

static void write_docker_name_field(FILE *f, const struct hold_run_record *r, const char *tail) {
    fprintf(f, "  \"Name\": \"");
    hold_json_escape(f, r && r->has_name ? r->name : "");
    fprintf(f, "\"%s", tail ? tail : "");
}

static void write_state_object(FILE *f, const struct hold_run_record *r, const char *indent, const char *tail) {
    const char *status = record_status(r);
    bool running = record_running(r);
    fprintf(f, "%s\"State\": {\n", indent);
    fprintf(f, "%s  \"Status\": \"", indent);
    hold_json_escape(f, status);
    fprintf(f, "\",\n");
    fprintf(f, "%s  \"Running\": %s,\n", indent, running ? "true" : "false");
    fprintf(f, "%s  \"Paused\": false,\n", indent);
    fprintf(f, "%s  \"Restarting\": false,\n", indent);
    fprintf(f, "%s  \"Dead\": false,\n", indent);
    fprintf(f, "%s  \"Pid\": %ld,\n", indent, (long)(r ? r->pid : 0));
    fprintf(f, "%s  \"Pgid\": %ld,\n", indent, (long)(r ? r->pgid : 0));
    fprintf(f, "%s  \"Sid\": %ld,\n", indent, (long)(r ? r->sid : 0));
    fprintf(f, "%s  \"ExitCode\": %d,\n", indent, (r && r->has_exit_code) ? r->exit_code : 0);
    fprintf(f, "%s  \"Error\": \"", indent);
    if (r && r->has_launch_error) hold_json_escape(f, r->launch_error);
    fprintf(f, "\",\n");
    fprintf(f, "%s  \"StartedAt\": \"", indent);
    hold_json_escape(f, r && r->has_started_at && r->started_at[0] ? r->started_at : "0001-01-01T00:00:00Z");
    fprintf(f, "\",\n");
    fprintf(f, "%s  \"FinishedAt\": \"", indent);
    hold_json_escape(f, r && r->has_ended_at && r->ended_at[0] ? r->ended_at : "0001-01-01T00:00:00Z");
    fprintf(f, "\"\n");
    fprintf(f, "%s}%s", indent, tail ? tail : "");
}

static void write_docker_config_object(FILE *f, const struct hold_run_record *r, bool public_projection) {
    fprintf(f, "  \"Config\": {\n");
    if (public_projection) {
        if (r && r->has_alias) {
            fprintf(f, "    \"Origin\": \"");
            hold_json_escape(f, r->alias);
            fprintf(f, "\"\n");
        }
        fprintf(f, "  }");
        return;
    }
    fprintf(f, "    \"User\": \"\",\n");
    fprintf(f, "    \"AttachStdin\": %s,\n", r->attach_stdin ? "true" : "false");
    fprintf(f, "    \"AttachStdout\": %s,\n", r->attach_stdout ? "true" : "false");
    fprintf(f, "    \"AttachStderr\": %s,\n", r->attach_stderr ? "true" : "false");
    fprintf(f, "    \"Tty\": %s,\n", r->tty ? "true" : "false");
    fprintf(f, "    \"OpenStdin\": %s,\n", r->open_stdin ? "true" : "false");
    fprintf(f, "    \"StdinOnce\": %s,\n", r->stdin_once ? "true" : "false");
    fprintf(f, "    \"Env\": ");
    if (r->envc > 0 && r->env) {
        hold_write_json_argv(f, r->envc, r->env);
    } else {
        fputs("[]", f);
    }
    fprintf(f, ",\n");
    if (r->has_alias) {
        fprintf(f, "    \"Origin\": \"");
        hold_json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
    fprintf(f, "    \"WorkingDir\": \"");
    if (r->has_observed && r->observed_cwd[0] == '/') {
        hold_json_escape(f, r->observed_cwd);
    }
    fprintf(f, "\",\n");
    fprintf(f, "    \"CapAdd\": ");
    if (r->cap_addc > 0 && r->cap_add) hold_write_json_argv(f, r->cap_addc, r->cap_add);
    else fputs("null", f);
    fprintf(f, ",\n");
    fprintf(f, "    \"CapDrop\": ");
    if (r->cap_dropc > 0 && r->cap_drop) hold_write_json_argv(f, r->cap_dropc, r->cap_drop);
    else fputs("null", f);
    fprintf(f, ",\n");
    fprintf(f, "    \"Privileged\": %s,\n", r->uid == 0 ? "true" : "false");
    fprintf(f, "    \"LogConfig\": {\"Type\": \"");
    hold_json_escape(f, r->has_log_destination && r->log_destination[0] ? r->log_destination : "local");
    fprintf(f, "\", \"Config\": {}}\n");
    fprintf(f, "  }");
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

    fprintf(f, "{\n");
    write_docker_identity_fields(f, r, record_created_timestamp(r, "0001-01-01T00:00:00Z"));
    fprintf(f, "  \"Path\": \"");
    hold_json_escape(f, argc > 0 && argv && argv[0] ? argv[0] : "");
    fprintf(f, "\",\n");
    fprintf(f, "  \"Args\": ");
    write_json_argv_tail(f, argc, argv);
    fprintf(f, ",\n");
    write_state_object(f, r, "  ", ",\n");
    if (r->has_log) {
        fprintf(f, "  \"LogPath\": \"");
        hold_json_escape(f, r->log_path);
        fprintf(f, "\",\n");
        char docker_idx_path[HOLD_PATH_MAX];
        if (hold_log_idx_path(r->log_path, docker_idx_path, sizeof(docker_idx_path)) == 0) {
            fprintf(f, "  \"LogIdx\": \"");
            hold_json_escape(f, docker_idx_path);
            fprintf(f, "\",\n");
        }
    }
    write_docker_name_field(f, r, ",\n");
    fprintf(f, "  \"RestartCount\": 0,\n");
    write_docker_config_object(f, r, false);
    fprintf(f, ",\n");
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
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        hold_json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
    if (r->has_name) {
        fprintf(f, "  \"name\": \"");
        hold_json_escape(f, r->name);
        fprintf(f, "\",\n");
    }
    if (r->has_started_at) {
        fprintf(f, "  \"started_at\": \"");
        hold_json_escape(f, r->started_at);
        fprintf(f, "\",\n");
    }
    if (r->has_created_at) {
        fprintf(f, "  \"created_at\": \"");
        hold_json_escape(f, r->created_at);
        fprintf(f, "\",\n");
    }
    if (r->has_ended_at) {
        fprintf(f, "  \"ended_at\": \"");
        hold_json_escape(f, r->ended_at);
        fprintf(f, "\",\n");
    }
    if (r->has_state) {
        fprintf(f, "  \"state\": \"");
        hold_json_escape(f, r->state);
        fprintf(f, "\",\n");
    }
    if (r->has_exit_code) {
        fprintf(f, "  \"exit_code\": %d,\n", r->exit_code);
    }
    if (r->has_term_signal) {
        fprintf(f, "  \"term_signal\": %d,\n", r->term_signal);
    }
    if (r->has_launch_error) {
        fprintf(f, "  \"launch_error\": \"");
        hold_json_escape(f, r->launch_error);
        fprintf(f, "\",\n");
    }
    if (r->has_console) {
        fprintf(f, "  \"console_sock\": \"");
        hold_json_escape(f, r->console_sock);
        fprintf(f, "\",\n");
    }
    if (r->envc > 0 && r->env) {
        fprintf(f, "  \"env\": ");
        hold_write_json_argv(f, r->envc, r->env);
        fprintf(f, ",\n");
    }
    if (r->portc > 0 && r->ports) {
        fprintf(f, "  \"ports\": ");
        hold_write_json_argv(f, r->portc, r->ports);
        fprintf(f, ",\n");
    }
    if (r->volumec > 0 && r->volumes) {
        fprintf(f, "  \"volumes\": ");
        hold_write_json_argv(f, r->volumec, r->volumes);
        fprintf(f, ",\n");
    }
    if (r->cap_addc > 0 && r->cap_add) {
        fprintf(f, "  \"cap_add\": ");
        hold_write_json_argv(f, r->cap_addc, r->cap_add);
        fprintf(f, ",\n");
    }
    if (r->cap_dropc > 0 && r->cap_drop) {
        fprintf(f, "  \"cap_drop\": ");
        hold_write_json_argv(f, r->cap_dropc, r->cap_drop);
        fprintf(f, ",\n");
    }
    if (r->mode_interactive || r->mode_tty || r->mode_detach || r->allow_multi) {
        fprintf(f, "  \"mode\": {");
        bool wrote_mode = false;
        if (r->mode_interactive) { fprintf(f, "%s\"interactive\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->mode_tty) { fprintf(f, "%s\"tty\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->mode_detach) { fprintf(f, "%s\"detach\": true", wrote_mode ? ", " : ""); wrote_mode = true; }
        if (r->allow_multi) { fprintf(f, "%s\"allow_multi\": true", wrote_mode ? ", " : ""); }
        fprintf(f, "},\n");
    }
    if (r->has_restart_policy && r->restart_policy[0]) {
        fprintf(f, "  \"restart\": \"");
        hold_json_escape(f, r->restart_policy);
        fprintf(f, "\",\n");
    }
    if (r->has_restart_delay) {
        fprintf(f, "  \"restart_delay_seconds\": %d,\n", r->restart_delay_seconds);
    }
    if (r->has_log_destination && r->log_destination[0]) {
        fprintf(f, "  \"log_destination\": \"");
        hold_json_escape(f, r->log_destination);
        fprintf(f, "\",\n");
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
    if (r->has_boot) {
        fprintf(f, "  \"boot_id\": \"");
        hold_json_escape(f, r->boot_id);
        fprintf(f, "\",\n");
    }
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

int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r) {
    char tmp[HOLD_PATH_MAX], fin[HOLD_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (hold_checked_snprintf(fin, sizeof(fin), "%s/%s.json", store->public_dir, r->id) != 0 ||
        hold_checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", store->public_dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0644) != 0) {
        goto out;
    }
    if (geteuid() == 0 && fchown(fd, 0, 0) != 0) {
        goto out;
    }
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        fd = -1;
        goto out;
    }

    const char *created = record_created_timestamp(r, "-");

    fprintf(f, "{\n");
    write_docker_identity_fields(f, r, created);
    write_docker_name_field(f, r, ",\n");
    write_docker_config_object(f, r, true);
    fprintf(f, ",\n");
    fprintf(f, "  \"id\": \"");
    hold_json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"root_managed\": true,\n");
    fprintf(f, "  \"requires_elevation\": true,\n");
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        hold_json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
    if (r->has_name) {
        fprintf(f, "  \"name\": \"");
        hold_json_escape(f, r->name);
        fprintf(f, "\",\n");
    }
    fprintf(f, "  \"state_hint\": \"");
    hold_json_escape(f, record_status(r));
    fprintf(f, "\",\n");
    fprintf(f, "  \"started_at\": \"");
    hold_json_escape(f, r->has_started_at && r->started_at[0] ? r->started_at : "-");
    fprintf(f, "\",\n");
    fprintf(f, "  \"created_at\": \"");
    hold_json_escape(f, created);
    fprintf(f, "\",\n");
    write_state_object(f, r, "  ", "\n");
    fprintf(f, "}\n");

    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
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
    int dfd = open(store->public_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0) {
            fprintf(stderr, "hold: warning: failed to fsync public index dir: %s\n", strerror(errno));
        }
        close(dfd);
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
    const char *state_obj = NULL;
    (void)hold_json_find_key(j, "State", &state_obj);
    if (hold_json_get_i64(j, "pid", &tmp) != 0) {
        if (!state_obj || *state_obj != '{' || hold_json_get_i64(state_obj, "Pid", &tmp) != 0) {
            free(j);
            return -1;
        }
    }
    r->pid = (pid_t)tmp;
    if (hold_json_get_i64(j, "pgid", &tmp) != 0) {
        if (!state_obj || *state_obj != '{' || hold_json_get_i64(state_obj, "Pgid", &tmp) != 0) {
            free(j);
            return -1;
        }
    }
    r->pgid = (pid_t)tmp;
    if (hold_json_get_i64(j, "sid", &tmp) != 0) {
        if (!state_obj || *state_obj != '{' || hold_json_get_i64(state_obj, "Sid", &tmp) != 0) {
            free(j);
            return -1;
        }
    }
    r->sid = (pid_t)tmp;
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
    r->uid = (uid_t)tmp;
    if (hold_json_get_i64(j, "gid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->gid = (gid_t)tmp;
    if (hold_json_get_i64(j, "invoked_by_uid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_uid = (uid_t)tmp;
    }
    if (hold_json_get_i64(j, "invoked_by_gid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_gid = (gid_t)tmp;
    }
    if (hold_json_get_str(j, "invoked_by_user", r->invoked_by_user, sizeof(r->invoked_by_user)) == 0) {
        r->has_invocation = true;
    }
    if (hold_json_get_bool(j, "invoked_via_sudo", &r->invoked_via_sudo) == 0) {
        r->has_invocation = true;
    }
    if ((hold_json_get_str(j, "alias", r->alias, sizeof(r->alias)) == 0 ||
         hold_json_get_str(j, "Origin", r->alias, sizeof(r->alias)) == 0) &&
        hold_valid_alias(r->alias)) {
        r->has_alias = true;
    }
    if (hold_json_get_str(j, "name", r->name, sizeof(r->name)) == 0 && hold_valid_alias(r->name)) {
        r->has_name = true;
    } else if (hold_json_get_str(j, "Name", r->name, sizeof(r->name)) == 0 && hold_valid_alias(r->name)) {
        r->has_name = true;
    }
    if (hold_json_get_str(j, "log_path", r->log_path, sizeof(r->log_path)) == 0) {
        r->has_log = true;
    } else if (hold_json_get_str(j, "LogPath", r->log_path, sizeof(r->log_path)) == 0) {
        r->has_log = true;
    }
    if (hold_json_get_str(j, "boot_id", r->boot_id, sizeof(r->boot_id)) == 0) {
        r->has_boot = true;
    }
    if (hold_json_get_str(j, "started_at", r->started_at, sizeof(r->started_at)) == 0) {
        r->has_started_at = true;
    } else if (state_obj && *state_obj == '{' &&
               hold_json_get_str(state_obj, "StartedAt", r->started_at, sizeof(r->started_at)) == 0) {
        r->has_started_at = true;
    }
    if (hold_json_get_str(j, "created_at", r->created_at, sizeof(r->created_at)) == 0) {
        r->has_created_at = true;
    } else if (hold_json_get_str(j, "Created", r->created_at, sizeof(r->created_at)) == 0) {
        r->has_created_at = true;
    } else if (r->has_started_at) {
        snprintf(r->created_at, sizeof(r->created_at), "%s", r->started_at);
        r->has_created_at = true;
    }
    if (hold_json_get_str(j, "ended_at", r->ended_at, sizeof(r->ended_at)) == 0) {
        r->has_ended_at = true;
    } else if (state_obj && *state_obj == '{' &&
               hold_json_get_str(state_obj, "FinishedAt", r->ended_at, sizeof(r->ended_at)) == 0) {
        r->has_ended_at = true;
    }
    if (hold_json_get_str(j, "state", r->state, sizeof(r->state)) == 0) {
        r->has_state = true;
    } else if (state_obj && *state_obj == '{' &&
               hold_json_get_str(state_obj, "Status", r->state, sizeof(r->state)) == 0) {
        r->has_state = true;
    }
    if (hold_json_get_i64(j, "exit_code", &tmp) == 0) {
        r->has_exit_code = true;
        r->exit_code = (int)tmp;
    } else if (state_obj && *state_obj == '{' &&
               hold_json_get_i64(state_obj, "ExitCode", &tmp) == 0) {
        r->has_exit_code = true;
        r->exit_code = (int)tmp;
    }
    if (hold_json_get_i64(j, "term_signal", &tmp) == 0) {
        r->has_term_signal = true;
        r->term_signal = (int)tmp;
    }
    if (hold_json_get_str(j, "launch_error", r->launch_error, sizeof(r->launch_error)) == 0) {
        r->has_launch_error = true;
    } else if (state_obj && *state_obj == '{' &&
               hold_json_get_str(state_obj, "Error", r->launch_error, sizeof(r->launch_error)) == 0 &&
               r->launch_error[0]) {
        r->has_launch_error = true;
    }
    if (hold_json_get_str(j, "console_sock", r->console_sock, sizeof(r->console_sock)) == 0 &&
        r->console_sock[0] == '/') {
        r->has_console = true;
    }
    const char *config_obj = NULL;
    if (hold_json_find_key(j, "Config", &config_obj) == 0 && config_obj && *config_obj == '{') {
        bool b = false;
        r->has_stdio_config = true;
        if (hold_json_get_bool(config_obj, "AttachStdin", &b) == 0) r->attach_stdin = b;
        if (hold_json_get_bool(config_obj, "AttachStdout", &b) == 0) r->attach_stdout = b;
        if (hold_json_get_bool(config_obj, "AttachStderr", &b) == 0) r->attach_stderr = b;
        if (hold_json_get_bool(config_obj, "Tty", &b) == 0) r->tty = b;
        if (hold_json_get_bool(config_obj, "OpenStdin", &b) == 0) r->open_stdin = b;
        if (hold_json_get_bool(config_obj, "StdinOnce", &b) == 0) r->stdin_once = b;
        if (!r->has_alias &&
            hold_json_get_str(config_obj, "Origin", r->alias, sizeof(r->alias)) == 0 &&
            hold_valid_alias(r->alias)) {
            r->has_alias = true;
        }
    } else {
        r->attach_stdout = true;
        r->attach_stderr = true;
        r->tty = r->has_console;
        r->open_stdin = r->has_console;
    }
    const char *mode_obj = NULL;
    if (hold_json_find_key(j, "mode", &mode_obj) == 0 && mode_obj && *mode_obj == '{') {
        bool b = false;
        if (hold_json_get_bool(mode_obj, "interactive", &b) == 0) r->mode_interactive = b;
        if (hold_json_get_bool(mode_obj, "tty", &b) == 0) r->mode_tty = b;
        if (hold_json_get_bool(mode_obj, "detach", &b) == 0) r->mode_detach = b;
        if (hold_json_get_bool(mode_obj, "allow_multi", &b) == 0) r->allow_multi = b;
    } else {
        r->mode_interactive = r->open_stdin && !r->tty;
        r->mode_tty = r->tty;
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
    if (config_obj && *config_obj == '{' &&
        (!r->has_observed || r->observed_cwd[0] == '\0')) {
        if (hold_json_get_str(config_obj, "WorkingDir", r->observed_cwd, sizeof(r->observed_cwd)) == 0 &&
            r->observed_cwd[0]) {
            r->has_observed = true;
        }
    }
    if (!r->has_observed || r->observed_exe[0] == '\0') {
        if (hold_json_get_str(j, "Path", r->observed_exe, sizeof(r->observed_exe)) == 0 &&
            r->observed_exe[0]) {
            r->has_observed = true;
        }
    }
    if (hold_json_get_argv_alloc(j, &r->argv, &r->argc) != 0) {
        if (hold_json_get_path_args_argv_alloc(j, &r->argv, &r->argc) != 0) {
            r->argv = NULL;
            r->argc = 0;
        }
    }
    if (hold_json_get_env_alloc(j, &r->env, &r->envc) != 0) {
        if (!config_obj || *config_obj != '{' ||
            hold_json_get_string_array_key_allow_empty_alloc(config_obj, "Env", &r->env, &r->envc) != 0) {
            r->env = NULL;
            r->envc = 0;
        }
    }
    if (hold_json_get_ports_alloc(j, &r->ports, &r->portc) != 0) {
        r->ports = NULL;
        r->portc = 0;
    }
    if (hold_json_get_volumes_alloc(j, &r->volumes, &r->volumec) != 0) {
        r->volumes = NULL;
        r->volumec = 0;
    }
    if (hold_json_get_string_array_key_alloc(j, "cap_add", &r->cap_add, &r->cap_addc) != 0 &&
        hold_json_get_string_array_key_alloc(j, "CapAdd", &r->cap_add, &r->cap_addc) != 0 &&
        (!config_obj || *config_obj != '{' ||
         hold_json_get_string_array_key_alloc(config_obj, "CapAdd", &r->cap_add, &r->cap_addc) != 0)) {
        r->cap_add = NULL;
        r->cap_addc = 0;
    }
    if (hold_json_get_string_array_key_alloc(j, "cap_drop", &r->cap_drop, &r->cap_dropc) != 0 &&
        hold_json_get_string_array_key_alloc(j, "CapDrop", &r->cap_drop, &r->cap_dropc) != 0 &&
        (!config_obj || *config_obj != '{' ||
         hold_json_get_string_array_key_alloc(config_obj, "CapDrop", &r->cap_drop, &r->cap_dropc) != 0)) {
        r->cap_drop = NULL;
        r->cap_dropc = 0;
    }
    if (hold_json_get_str(j, "restart", r->restart_policy, sizeof(r->restart_policy)) == 0 &&
        r->restart_policy[0]) {
        r->has_restart_policy = true;
    }
    if (hold_json_get_i64(j, "restart_delay_seconds", &tmp) == 0 && tmp >= 0 && tmp <= INT_MAX) {
        r->restart_delay_seconds = (int)tmp;
        r->has_restart_delay = true;
    }
    if (hold_json_get_str(j, "log_destination", r->log_destination, sizeof(r->log_destination)) == 0 &&
        r->log_destination[0]) {
        r->has_log_destination = true;
    } else if (config_obj && *config_obj == '{') {
        const char *log_config = NULL;
        if (hold_json_find_key(config_obj, "LogConfig", &log_config) == 0 && log_config && *log_config == '{' &&
            hold_json_get_str(log_config, "Type", r->log_destination, sizeof(r->log_destination)) == 0 &&
            r->log_destination[0]) {
            r->has_log_destination = true;
        }
    }
    if (r->has_log_destination && strcmp(r->log_destination, "local") == 0) {
        r->log_destination[0] = '\0';
        r->has_log_destination = false;
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
        if (r->argc <= 0 || !r->argv || hold_format_argv_human(r->cmdline, sizeof(r->cmdline), r->argc, r->argv) != 0) {
            snprintf(r->cmdline, sizeof(r->cmdline), "?");
        }
    }
    free(j);
    return 0;
}

int hold_load_public_index(const char *path, struct hold_public_index *pi) {
    memset(pi, 0, sizeof(*pi));
    char *j = NULL;
    if (hold_read_small_file(path, &j) != 0) {
        return -1;
    }
    if ((hold_json_get_str(j, "id", pi->id, sizeof(pi->id)) != 0 &&
         hold_json_get_str(j, "Id", pi->id, sizeof(pi->id)) != 0) ||
        !hold_valid_id(pi->id)) {
        free(j);
        return -1;
    }
    if (hold_json_get_bool(j, "root_managed", &pi->root_managed) != 0) {
        pi->root_managed = true;
    }
    if (hold_json_get_bool(j, "requires_elevation", &pi->requires_elevation) != 0) {
        pi->requires_elevation = true;
    }
    if ((hold_json_get_str(j, "alias", pi->alias, sizeof(pi->alias)) == 0 ||
         hold_json_get_str(j, "Origin", pi->alias, sizeof(pi->alias)) == 0) &&
        hold_valid_alias(pi->alias)) {
        pi->has_alias = true;
    }
    if (!pi->has_alias) {
        const char *config_obj = NULL;
        if (hold_json_find_key(j, "Config", &config_obj) == 0 && config_obj && *config_obj == '{' &&
            hold_json_get_str(config_obj, "Origin", pi->alias, sizeof(pi->alias)) == 0 &&
            hold_valid_alias(pi->alias)) {
            pi->has_alias = true;
        }
    }
    if ((hold_json_get_str(j, "name", pi->name, sizeof(pi->name)) == 0 ||
         hold_json_get_str(j, "Name", pi->name, sizeof(pi->name)) == 0) &&
        hold_valid_alias(pi->name)) {
        pi->has_name = true;
    }
    if (hold_json_get_str(j, "state_hint", pi->state_hint, sizeof(pi->state_hint)) != 0) {
        snprintf(pi->state_hint, sizeof(pi->state_hint), "%s", "unknown");
    }
    if (hold_json_get_str(j, "started_at", pi->started_at, sizeof(pi->started_at)) != 0) {
        snprintf(pi->started_at, sizeof(pi->started_at), "%s", "-");
    }
    if (hold_json_get_str(j, "created_at", pi->created_at, sizeof(pi->created_at)) != 0 &&
        hold_json_get_str(j, "Created", pi->created_at, sizeof(pi->created_at)) != 0) {
        snprintf(pi->created_at, sizeof(pi->created_at), "%s", pi->started_at[0] ? pi->started_at : "-");
    }

    const char *state_obj = NULL;
    if (hold_json_find_key(j, "State", &state_obj) == 0 && state_obj && *state_obj == '{') {
        int64_t state_i64 = 0;
        bool state_bool = false;
        if (hold_json_get_str(state_obj, "Status", pi->state_hint, sizeof(pi->state_hint)) == 0) {
            pi->has_state = true;
        }
        if (hold_json_get_bool(state_obj, "Running", &state_bool) == 0) {
            pi->running = state_bool;
            pi->has_state = true;
        } else {
            pi->running = strcmp(pi->state_hint, "running") == 0;
        }
        if (hold_json_get_bool(state_obj, "Paused", &state_bool) == 0) {
            pi->paused = state_bool;
            pi->has_state = true;
        }
        if (hold_json_get_bool(state_obj, "Restarting", &state_bool) == 0) {
            pi->restarting = state_bool;
            pi->has_state = true;
        }
        if (hold_json_get_bool(state_obj, "Dead", &state_bool) == 0) {
            pi->dead = state_bool;
            pi->has_state = true;
        }
        if (hold_json_get_i64(state_obj, "Pid", &state_i64) == 0) {
            pi->pid = (pid_t)state_i64;
            pi->has_state = true;
        }
        if (hold_json_get_i64(state_obj, "Pgid", &state_i64) == 0) {
            pi->pgid = (pid_t)state_i64;
            pi->has_state = true;
        }
        if (hold_json_get_i64(state_obj, "Sid", &state_i64) == 0) {
            pi->sid = (pid_t)state_i64;
            pi->has_state = true;
        }
        if (hold_json_get_i64(state_obj, "ExitCode", &state_i64) == 0) {
            pi->exit_code = (int)state_i64;
            pi->has_exit_code = true;
            pi->has_state = true;
        }
        if (hold_json_get_str(state_obj, "Error", pi->error, sizeof(pi->error)) == 0) {
            pi->has_state = true;
        }
        if (hold_json_get_str(state_obj, "StartedAt", pi->started_at, sizeof(pi->started_at)) == 0) {
            pi->has_state = true;
        }
        if (hold_json_get_str(state_obj, "FinishedAt", pi->finished_at, sizeof(pi->finished_at)) == 0) {
            pi->has_state = true;
        }
    } else {
        pi->running = strcmp(pi->state_hint, "running") == 0;
    }
    free(j);
    return 0;
}

int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi) {
    if (!hold_valid_id(id)) {
        return -1;
    }
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) != 0) {
        return -1;
    }
    if (hold_load_public_index(path, pi) != 0 || strcmp(pi->id, id) != 0) {
        return -1;
    }
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
    bool have_old_st = stat(path, &old_st) == 0;

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

    int argc = r.argc;
    char **argv = r.argv;
    if (argc <= 0 || !argv) {
        argc = r.observed_argc;
        argv = r.observed_argv;
    }
    char rewritten_path[HOLD_PATH_MAX] = {0};
    int rc = hold_write_record_atomic(store->record_dir, &r, argc, argv, rewritten_path, sizeof(rewritten_path));
    if (rc == 0 && have_old_st && geteuid() == 0 && rewritten_path[0]) {
        if (chown(rewritten_path, old_st.st_uid, old_st.st_gid) != 0) {
            rc = -1;
        }
    }
    if (rc == 0 && store->kind == STORE_SYSTEM_MANAGED && store->public_dir[0]) {
        rc = hold_write_public_index_atomic(store, &r);
    }
    hold_free_run_record(&r);
    return rc;
}
