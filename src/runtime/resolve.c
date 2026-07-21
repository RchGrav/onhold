#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n);
static int resolve_user_store_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_private_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n);
static int append_resolved_target(struct hold_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct hold_store *store,
                                  const char *id,
                                  bool requires_root);
static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command);
static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name,
                                         const char *command);

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (hold_valid_id(input)) {
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", dir, input) == 0 && access(path, F_OK) == 0) {
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
        char id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, id, sizeof(id))) {
            continue;
        }
        if (strncmp(id, input, strlen(input)) == 0) {
            matches++;
            if (hold_checked_snprintf(resolved, n, "%s", id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return (matches == 1) ? 0 : -1;
}

enum id_token_scope hold_parse_id_token(const char *token, const char **id_out) {
    if (!token || !*token) {
        return ID_TOKEN_INVALID;
    }
    if (strncmp(token, "user:", 5) == 0) {
        *id_out = token + 5;
        return **id_out ? ID_TOKEN_USER : ID_TOKEN_INVALID;
    }
    if (strncmp(token, "system:", 7) == 0) {
        *id_out = token + 7;
        return **id_out ? ID_TOKEN_SYSTEM : ID_TOKEN_INVALID;
    }
    *id_out = token;
    return ID_TOKEN_PLAIN;
}

static int resolve_user_store_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_private_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_public_id(const struct hold_store *store, const char *id, char *resolved, size_t n) {
    if (resolve_run_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct hold_public_index pi;
    if (hold_load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

bool hold_record_matches_alias_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "view")) {
        return r->has_log;
    }
    if (!strcmp(command, "inspect")) {
        return true;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static bool hold_record_matches_run_name_intent(const char *command, const struct hold_run_record *r, enum run_state st) {
    if (!strcmp(command, "start-existing")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE || st == STATE_RUNNING;
    }
    return hold_record_matches_alias_intent(command, r, st);
}

static int append_resolved_target(struct hold_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct hold_store *store,
                                  const char *id,
                                  bool requires_root) {
    struct hold_resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    struct hold_resolved_target *out = &(*targets)[*count];
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->requires_root = requires_root;
    hold_checked_snprintf(out->id, sizeof(out->id), "%s", id);
    (*count)++;
    return 0;
}

static int append_private_run_name_target(struct hold_resolved_target **targets,
                                          int *count,
                                          enum resolve_scope scope,
                                          const struct hold_store *store,
                                          const char *name,
                                          const char *command) {
    if (!hold_valid_alias(name)) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0 || !hold_valid_record(&r) ||
            strcmp(r.id, file_id) != 0 || !r.has_name || strcmp(r.name, name) != 0) {
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, boot_id);
        if (hold_record_matches_run_name_intent(command, &r, st)) {
            matches++;
            snprintf(matched, sizeof(matched), "%s", r.id);
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: call name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, scope, store, matched, false) == 0 ? 1 : -1;
}

static int append_public_run_name_target(struct hold_resolved_target **targets,
                                         int *count,
                                         const struct hold_store *system_store,
                                         const char *name,
                                         const char *command) {
    (void)command;
    if (!hold_valid_alias(name)) return 0;
    DIR *d = opendir(system_store->public_dir);
    if (!d) return 0;
    char matched[ID_STR_LEN] = {0};
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) continue;
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= ID_STR_LEN) continue;
        char id[ID_STR_LEN];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!hold_valid_id(id)) continue;
        struct hold_public_index pi;
        if (hold_load_public_index_by_id(system_store, id, &pi) == 0 &&
            pi.has_name && strcmp(pi.name, name) == 0) {
            matches++;
            snprintf(matched, sizeof(matched), "%s", id);
        }
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: call name '%s' is ambiguous\n", name);
        return -2;
    }
    return append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, system_store, matched, true) == 0 ? 1 : -1;
}

int hold_report_not_found(const char *token) {
    fprintf(stderr, "hold: error: no call matches '%s'\n", token ? token : "");
    return 5;
}

int hold_report_requires_root(const char *token) {
    fprintf(stderr, "hold: error: '%s' is root-managed; operate on it as root\n", token ? token : "");
    return 3;
}

int hold_resolve_action_token(const struct hold_invocation *inv,
                                const struct hold_store *current_user_store,
                                const struct hold_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct hold_resolved_target **targets_out,
                                int *count_out) {
    (void)all;
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    if (scope == ID_TOKEN_INVALID || (!hold_valid_id_prefix(atom) && !hold_valid_alias(atom))) {
        fprintf(stderr, "hold: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    if (hold_valid_alias(atom)) {
        int name_rc = 0;
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct hold_store user_store;
                if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
            } else if (scope == ID_TOKEN_SYSTEM) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
            } else {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command);
                if (name_rc == 0 && inv->have_sudo_user) {
                    struct hold_store user_store;
                    if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                        name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command);
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                name_rc = append_private_run_name_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command);
            }
            if (name_rc == 0 && (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN)) {
                name_rc = append_public_run_name_target(targets_out, count_out, system_store, atom, command);
            }
        }
        if (name_rc == 1) return 0;
        if (name_rc == -2) return 6;
        if (name_rc < 0) return 3;
    }

    if (hold_valid_id_prefix(atom)) {
        char resolved[ID_STR_LEN];
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct hold_store user_store;
                if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                if (resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                }
            } else if (scope == ID_TOKEN_SYSTEM) {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
            } else {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
                if (inv->have_sudo_user) {
                    struct hold_store user_store;
                    if (hold_init_invoking_user_store(inv, &user_store) == 0 &&
                        resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                        return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                if (resolve_user_store_id(current_user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, resolved, false) == 0 ? 0 : 3;
                }
                if (scope == ID_TOKEN_USER) {
                    return hold_report_not_found(token);
                }
            }
            if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
                if (resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true) == 0 ? 0 : 3;
                }
            }
        }
    }

    return hold_report_not_found(token);
}
