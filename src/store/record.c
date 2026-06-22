#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/store.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"

int write_record_atomic(const char *dir, const struct record *r, int argc, char **argv, char *out_json_path, size_t out_n) {
    char tmp[SIGMUND_PATH_MAX], fin[SIGMUND_PATH_MAX], reserve[SIGMUND_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (checked_snprintf(fin, sizeof(fin), "%s/%s.json", dir, r->id) != 0) {
        return -1;
    }
    if (checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, r->id) != 0) {
        return -1;
    }
    if (checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
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
    fprintf(f, "  \"version\": %d,\n", r->version);
    fprintf(f, "  \"id\": \"");
    json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"run_id\": \"");
    json_escape(f, r->run_id[0] ? r->run_id : r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"pid\": %ld,\n", (long)r->pid);
    fprintf(f, "  \"pgid\": %ld,\n", (long)r->pgid);
    fprintf(f, "  \"sid\": %ld,\n", (long)r->sid);
    fprintf(f, "  \"start_unix_ns\": %" PRId64 ",\n", r->start_unix_ns);
    fprintf(f, "  \"argv\": ");
    write_json_argv(f, argc, argv);
    fprintf(f, ",\n");
    fprintf(f, "  \"cmdline_display\": \"");
    json_escape(f, r->cmdline);
    fprintf(f, "\",\n");
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
    if (r->has_started_at) {
        fprintf(f, "  \"started_at\": \"");
        json_escape(f, r->started_at);
        fprintf(f, "\",\n");
    }
    if (r->has_ended_at) {
        fprintf(f, "  \"ended_at\": \"");
        json_escape(f, r->ended_at);
        fprintf(f, "\",\n");
    }
    if (r->has_state) {
        fprintf(f, "  \"state\": \"");
        json_escape(f, r->state);
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
        json_escape(f, r->launch_error);
        fprintf(f, "\",\n");
    }
    if (r->has_console) {
        fprintf(f, "  \"console_sock\": \"");
        json_escape(f, r->console_sock);
        fprintf(f, "\",\n");
    }
    fprintf(f, "  \"uid\": %u,\n", r->uid);
    fprintf(f, "  \"gid\": %u,\n", r->gid);
    if (r->has_invocation) {
        fprintf(f, "  \"invoked_by_uid\": %u,\n", r->invoked_by_uid);
        fprintf(f, "  \"invoked_by_gid\": %u,\n", r->invoked_by_gid);
        fprintf(f, "  \"invoked_by_user\": \"");
        json_escape(f, r->invoked_by_user);
        fprintf(f, "\",\n");
        fprintf(f, "  \"invoked_via_sudo\": %s,\n", r->invoked_via_sudo ? "true" : "false");
    }
    if (r->has_log) {
        fprintf(f, "  \"log_path\": \"");
        json_escape(f, r->log_path);
        fprintf(f, "\",\n");
    }
    if (r->has_boot) {
        fprintf(f, "  \"boot_id\": \"");
        json_escape(f, r->boot_id);
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
            fprintf(stderr, "sigmund: warning: failed to fsync storage dir: %s\n", strerror(errno));
        }
        close(dfd);
    }
    if (out_json_path && checked_snprintf(out_json_path, out_n, "%s", fin) != 0) {
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

int write_public_index_atomic(const struct store_paths *store, const struct record *r) {
    char tmp[SIGMUND_PATH_MAX], fin[SIGMUND_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (checked_snprintf(fin, sizeof(fin), "%s/%s.json", store->public_dir, r->id) != 0 ||
        checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", store->public_dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
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

    fprintf(f, "{\n");
    fprintf(f, "  \"id\": \"");
    json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"root_managed\": true,\n");
    fprintf(f, "  \"requires_elevation\": true,\n");
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
    fprintf(f, "  \"state_hint\": \"unknown\",\n");
    fprintf(f, "  \"started_at\": \"");
    json_escape(f, r->has_started_at && r->started_at[0] ? r->started_at : "-");
    fprintf(f, "\"\n");
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
    if (chmod(fin, 0644) != 0 || chown_root_if_root(fin) != 0) {
        goto out;
    }
    int dfd = open(store->public_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0) {
            fprintf(stderr, "sigmund: warning: failed to fsync public index dir: %s\n", strerror(errno));
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

int load_record(const char *path, struct record *r) {
    memset(r, 0, sizeof(*r));
    char *j = NULL;
    if (read_owned_file_no_symlink(path, &j) != 0) {
        return -1;
    }

    int64_t tmp = 0;
    if (json_get_i64(j, "version", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->version = (int)tmp;
    if (json_get_str(j, "id", r->id, sizeof(r->id)) != 0) {
        free(j);
        return -1;
    }
    if (json_get_str(j, "run_id", r->run_id, sizeof(r->run_id)) != 0) {
        snprintf(r->run_id, sizeof(r->run_id), "%s", r->id);
    }
    if (json_get_i64(j, "pid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->pid = (pid_t)tmp;
    if (json_get_i64(j, "pgid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->pgid = (pid_t)tmp;
    if (json_get_i64(j, "sid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->sid = (pid_t)tmp;
    if (json_get_i64(j, "start_unix_ns", &r->start_unix_ns) != 0) {
        free(j);
        return -1;
    }
    if (json_get_i64(j, "uid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->uid = (uid_t)tmp;
    if (json_get_i64(j, "gid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->gid = (gid_t)tmp;
    if (json_get_i64(j, "invoked_by_uid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_uid = (uid_t)tmp;
    }
    if (json_get_i64(j, "invoked_by_gid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_gid = (gid_t)tmp;
    }
    if (json_get_str(j, "invoked_by_user", r->invoked_by_user, sizeof(r->invoked_by_user)) == 0) {
        r->has_invocation = true;
    }
    if (json_get_bool(j, "invoked_via_sudo", &r->invoked_via_sudo) == 0) {
        r->has_invocation = true;
    }
    if (json_get_str(j, "alias", r->alias, sizeof(r->alias)) == 0 && valid_alias(r->alias)) {
        r->has_alias = true;
    }
    if (json_get_str(j, "log_path", r->log_path, sizeof(r->log_path)) == 0) {
        r->has_log = true;
    }
    if (json_get_str(j, "boot_id", r->boot_id, sizeof(r->boot_id)) == 0) {
        r->has_boot = true;
    }
    if (json_get_str(j, "started_at", r->started_at, sizeof(r->started_at)) == 0) {
        r->has_started_at = true;
    }
    if (json_get_str(j, "ended_at", r->ended_at, sizeof(r->ended_at)) == 0) {
        r->has_ended_at = true;
    }
    if (json_get_str(j, "state", r->state, sizeof(r->state)) == 0) {
        r->has_state = true;
    }
    if (json_get_i64(j, "exit_code", &tmp) == 0) {
        r->has_exit_code = true;
        r->exit_code = (int)tmp;
    }
    if (json_get_i64(j, "term_signal", &tmp) == 0) {
        r->has_term_signal = true;
        r->term_signal = (int)tmp;
    }
    if (json_get_str(j, "launch_error", r->launch_error, sizeof(r->launch_error)) == 0) {
        r->has_launch_error = true;
    }
    if (json_get_str(j, "console_sock", r->console_sock, sizeof(r->console_sock)) == 0 &&
        r->console_sock[0] == '/') {
        r->has_console = true;
    }
    if (json_get_u64(j, "proc_starttime_ticks", &r->proc_starttime_ticks) != 0 ||
        json_get_u64(j, "exe_dev", &r->exe_dev) != 0 ||
        json_get_u64(j, "exe_ino", &r->exe_ino) != 0) {
        free(j);
        return -1;
    }
    if (json_get_argv_display(j, r->cmdline, sizeof(r->cmdline)) != 0 &&
        json_get_str(j, "cmdline_display", r->cmdline, sizeof(r->cmdline)) != 0) {
        snprintf(r->cmdline, sizeof(r->cmdline), "?");
    }
    free(j);
    return 0;
}

int load_public_index(const char *path, struct public_index *pi) {
    memset(pi, 0, sizeof(*pi));
    char *j = NULL;
    if (read_small_file(path, &j) != 0) {
        return -1;
    }
    if (json_get_str(j, "id", pi->id, sizeof(pi->id)) != 0 || !valid_id(pi->id)) {
        free(j);
        return -1;
    }
    if (json_get_bool(j, "root_managed", &pi->root_managed) != 0) {
        pi->root_managed = true;
    }
    if (json_get_bool(j, "requires_elevation", &pi->requires_elevation) != 0) {
        pi->requires_elevation = true;
    }
    if (json_get_str(j, "alias", pi->alias, sizeof(pi->alias)) == 0 && valid_alias(pi->alias)) {
        pi->has_alias = true;
    }
    if (json_get_str(j, "state_hint", pi->state_hint, sizeof(pi->state_hint)) != 0) {
        snprintf(pi->state_hint, sizeof(pi->state_hint), "%s", "unknown");
    }
    if (json_get_str(j, "started_at", pi->started_at, sizeof(pi->started_at)) != 0) {
        snprintf(pi->started_at, sizeof(pi->started_at), "%s", "-");
    }
    free(j);
    return 0;
}

int load_public_index_by_id(const struct store_paths *store, const char *id, struct public_index *pi) {
    if (!valid_id(id)) {
        return -1;
    }
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) != 0) {
        return -1;
    }
    if (load_public_index(path, pi) != 0 || strcmp(pi->id, id) != 0) {
        return -1;
    }
    return 0;
}

int load_record_by_id(const char *dir, const char *id, struct record *r, char *path, size_t n) {
    if (!valid_id(id)) {
        return -1;
    }
    if (checked_snprintf(path, n, "%s/%s.json", dir, id) != 0) {
        return -1;
    }
    if (load_record(path, r) != 0 || !valid_record(r) || strcmp(r->id, id) != 0) {
        return -1;
    }
    return 0;
}
