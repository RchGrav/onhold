#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "record_internal.h"

/* The public projection: the deliberately narrower, world-readable view
 * of a root-managed call. Never carries argv, environment, or the owning
 * user. Writer and reader live together so the schema has one home. */

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

int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r,
                                     const char *observed_ports_csv) {
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

    /* The public projection is the one deliberately narrower schema: what
     * every user may see about a global call. Command line, environment,
     * and owning user are never written here. */
    fprintf(f, "{\n");
    fprintf(f, "  \"id\": \"");
    hold_json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"root_managed\": true,\n");
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
    /* Observed ports are root's projection of the live process group, refreshed
     * whenever root lists; non-root `list -a` renders this field verbatim. An
     * absent field simply means root has not observed ports for this call yet. */
    if (observed_ports_csv && observed_ports_csv[0]) {
        fprintf(f, "  \"observed_ports\": \"");
        hold_json_escape(f, observed_ports_csv);
        fprintf(f, "\",\n");
    }
    if (r->has_ended_at && r->ended_at[0]) {
        fprintf(f, "  \"ended_at\": \"");
        hold_json_escape(f, r->ended_at);
        fprintf(f, "\",\n");
    }
    if (r->has_exit_code) {
        fprintf(f, "  \"exit_code\": %d,\n", r->exit_code);
    }
    fprintf(f, "  \"pid\": %ld,\n", (long)r->pid);
    fprintf(f, "  \"pgid\": %ld,\n", (long)r->pgid);
    fprintf(f, "  \"sid\": %ld,\n", (long)r->sid);
    fprintf(f, "  \"running\": %s\n", record_running(r) ? "true" : "false");
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

int hold_load_public_index(const char *path, struct hold_public_index *pi) {
    memset(pi, 0, sizeof(*pi));
    char *j = NULL;
    if (hold_read_small_file(path, &j) != 0) {
        return -1;
    }
    if (hold_json_get_str(j, "id", pi->id, sizeof(pi->id)) != 0 ||
        !hold_valid_id(pi->id)) {
        free(j);
        return -1;
    }
    if (hold_json_get_bool(j, "root_managed", &pi->root_managed) != 0) {
        pi->root_managed = true;
    }
    if (hold_json_get_str(j, "name", pi->name, sizeof(pi->name)) == 0 &&
        hold_valid_alias(pi->name)) {
        pi->has_name = true;
    }
    if (hold_json_get_str(j, "state_hint", pi->state_hint, sizeof(pi->state_hint)) != 0) {
        snprintf(pi->state_hint, sizeof(pi->state_hint), "%s", "unknown");
    }
    if (hold_json_get_str(j, "started_at", pi->started_at, sizeof(pi->started_at)) != 0) {
        snprintf(pi->started_at, sizeof(pi->started_at), "%s", "-");
    }
    if (hold_json_get_str(j, "created_at", pi->created_at, sizeof(pi->created_at)) != 0) {
        snprintf(pi->created_at, sizeof(pi->created_at), "%s", pi->started_at[0] ? pi->started_at : "-");
    }
    if (hold_json_get_str(j, "observed_ports", pi->observed_ports, sizeof(pi->observed_ports)) != 0) {
        pi->observed_ports[0] = '\0';
    }
    if (hold_json_get_str(j, "ended_at", pi->finished_at, sizeof(pi->finished_at)) == 0) {
        pi->has_state = true;
    }
    int64_t v = 0;
    if (hold_json_get_i64(j, "exit_code", &v) == 0) {
        pi->exit_code = (int)v;
        pi->has_exit_code = true;
        pi->has_state = true;
    }
    if (hold_json_get_i64(j, "pid", &v) == 0 && hold_record_checked_i64_to_pid(v, &pi->pid) == 0) {
        pi->has_state = true;
    }
    if (hold_json_get_i64(j, "pgid", &v) == 0 && hold_record_checked_i64_to_pid(v, &pi->pgid) == 0) {
        pi->has_state = true;
    }
    if (hold_json_get_i64(j, "sid", &v) == 0 && hold_record_checked_i64_to_pid(v, &pi->sid) == 0) {
        pi->has_state = true;
    }
    bool running = false;
    if (hold_json_get_bool(j, "running", &running) == 0) {
        pi->running = running;
        pi->has_state = true;
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
