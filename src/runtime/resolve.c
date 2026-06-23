#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

bool sigmund_command_accepts_target_tokens(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "tail") || !strcmp(command, "dump") ||
                       !strcmp(command, "prune") || !strcmp(command, "console"));
}

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n);
static int resolve_user_store_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_private_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n);
static int resolve_system_public_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n);
static void fill_target(struct sigmund_resolved_target *out,
                        enum resolve_scope scope,
                        const struct sigmund_store *store,
                        const char *id,
                        bool needs_elevation);
static int set_target_capability(struct sigmund_resolved_target *target, const char *alias, const char *hash);
static int append_alias_match(struct alias_match_list *list,
                              const struct sigmund_run_record *r,
                              enum run_state st,
                              const char *started_at);
static bool public_alias_visible(const struct sigmund_store *store, const char *alias);
static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list);
static int append_resolved_target(struct sigmund_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct sigmund_store *store,
                                  const char *id,
                                  bool needs_elevation);
static int append_capability_target(struct sigmund_resolved_target **targets,
                                    int *count,
                                    const struct sigmund_store *store,
                                    const char *runid_sel,
                                    const char *alias,
                                    const char *hash);
static int append_private_alias_targets(struct sigmund_resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct sigmund_store *store,
                                        const char *alias,
                                        const char *command,
                                        bool all);
static int collect_public_alias_matches(const struct sigmund_store *store,
                                        const char *alias,
                                        struct alias_match_list *list);
static int append_public_alias_elevation_target(struct sigmund_resolved_target **targets,
                                                int *count,
                                                const struct sigmund_store *system_store,
                                                const char *alias,
                                                const char *command,
                                                bool all);

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (sigmund_valid_id(input)) {
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s.json", dir, input) == 0 && access(path, F_OK) == 0) {
            return sigmund_checked_snprintf(resolved, n, "%s", input);
        }
    }
    if (!sigmund_valid_id_prefix(input)) {
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
        if (!sigmund_record_json_filename_id(e->d_name, id, sizeof(id))) {
            continue;
        }
        if (strncmp(id, input, strlen(input)) == 0) {
            matches++;
            if (sigmund_checked_snprintf(resolved, n, "%s", id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return (matches == 1) ? 0 : -1;
}

enum id_token_scope sigmund_parse_id_token(const char *token, const char **id_out) {
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

int sigmund_ensure_run_recorded_under_alias(const struct sigmund_store *store, const char *id, const char *alias) {
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return -1;
    }
    if (!r.has_alias || strcmp(r.alias, alias) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int resolve_user_store_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_private_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_public_id(const struct sigmund_store *store, const char *id, char *resolved, size_t n) {
    if (resolve_run_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct sigmund_public_index pi;
    if (sigmund_load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

int sigmund_resolve_public_profile_token(const struct sigmund_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]) {
    if (sigmund_valid_profile_hash(token)) {
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (sigmund_valid_alias(token) && sigmund_alias_lookup_hash(store, token, hash) == 0) {
        return 1;
    }
    return 0;
}

static void fill_target(struct sigmund_resolved_target *out,
                        enum resolve_scope scope,
                        const struct sigmund_store *store,
                        const char *id,
                        bool needs_elevation) {
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->needs_elevation = needs_elevation;
    sigmund_checked_snprintf(out->id, sizeof(out->id), "%s", id);
}

static int set_target_capability(struct sigmund_resolved_target *target, const char *alias, const char *hash) {
    if (!target || !sigmund_valid_alias(alias) || !sigmund_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    if (sigmund_checked_snprintf(target->cap_alias, sizeof(target->cap_alias), "%s", alias) != 0 ||
        sigmund_checked_snprintf(target->cap_hash, sizeof(target->cap_hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_capability = true;
    return 0;
}

void sigmund_free_alias_match_list(struct alias_match_list *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

bool sigmund_command_all_allowed(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "prune"));
}

bool sigmund_record_matches_alias_intent(const char *command, const struct sigmund_run_record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "dump") || !strcmp(command, "view")) {
        return r->has_log;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static int append_alias_match(struct alias_match_list *list,
                              const struct sigmund_run_record *r,
                              enum run_state st,
                              const char *started_at) {
    struct alias_match *next = realloc(list->items, (list->count + 1) * sizeof(*list->items));
    if (!next) {
        return -1;
    }
    list->items = next;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    if (sigmund_checked_snprintf(list->items[list->count].id, sizeof(list->items[list->count].id), "%s", r->id) != 0 ||
        sigmund_checked_snprintf(list->items[list->count].started_at,
                         sizeof(list->items[list->count].started_at),
                         "%s",
                         started_at && *started_at ? started_at : "-") != 0) {
        return -1;
    }
    list->items[list->count].state = st;
    list->count++;
    return 0;
}

int sigmund_collect_private_alias_matches(const struct sigmund_store *store,
                                         const char *alias,
                                         const char *command,
                                         struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!sigmund_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = sigmund_alias_exists_in_store(store, alias);
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!sigmund_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            closedir(d);
            return -1;
        }
        struct sigmund_run_record r;
        if (sigmund_load_record(path, &r) != 0 || !sigmund_valid_record(&r) ||
            strcmp(r.id, file_id) != 0 || !r.has_alias || strcmp(r.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
        if (!sigmund_record_matches_alias_intent(command, &r, st)) {
            continue;
        }
        char started_at[64];
        if (r.has_started_at && r.started_at[0]) {
            snprintf(started_at, sizeof(started_at), "%s", r.started_at);
        } else {
            sigmund_format_rfc3339_utc_from_ns(r.start_unix_ns, started_at, sizeof(started_at));
        }
        if (append_alias_match(list, &r, st, started_at) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static bool public_alias_visible(const struct sigmund_store *store, const char *alias) {
    if (sigmund_alias_exists_in_store(store, alias)) {
        return true;
    }
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return false;
    }
    bool found = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!sigmund_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!sigmund_valid_id(id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (sigmund_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct sigmund_public_index pi;
        if (sigmund_load_public_index(path, &pi) == 0 && pi.has_alias && strcmp(pi.alias, alias) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list) {
    fprintf(stderr,
            "sigmund: error: alias '%s' matches more than one %s candidate\n",
            alias,
            command ? command : "target");
    fprintf(stderr, "sigmund: candidates:\n");
    for (size_t i = 0; i < list->count; i++) {
        fprintf(stderr,
                "  %s %-8s %s\n",
                list->items[i].id,
                sigmund_state_str(list->items[i].state),
                list->items[i].started_at);
    }
    if (sigmund_command_all_allowed(command)) {
        fprintf(stderr, "sigmund: use --all to apply %s to every listed run\n", command);
    }
}

static int append_resolved_target(struct sigmund_resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct sigmund_store *store,
                                  const char *id,
                                  bool needs_elevation) {
    struct sigmund_resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    fill_target(&(*targets)[*count], scope, store, id, needs_elevation);
    (*count)++;
    return 0;
}

static int append_capability_target(struct sigmund_resolved_target **targets,
                                    int *count,
                                    const struct sigmund_store *store,
                                    const char *runid_sel,
                                    const char *alias,
                                    const char *hash) {
    if (append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, store, runid_sel, true) != 0) {
        return -1;
    }
    if (set_target_capability(&(*targets)[*count - 1], alias, hash) != 0) {
        return -1;
    }
    return 0;
}

static int append_private_alias_targets(struct sigmund_resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct sigmund_store *store,
                                        const char *alias,
                                        const char *command,
                                        bool all) {
    struct alias_match_list matches;
    if (sigmund_collect_private_alias_matches(store, alias, command, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        sigmund_free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count > 1 && (!all || !sigmund_command_all_allowed(command))) {
        report_alias_ambiguity(command, alias, &matches);
        sigmund_free_alias_match_list(&matches);
        return -2;
    }
    for (size_t i = 0; i < matches.count; i++) {
        if (append_resolved_target(targets, count, scope, store, matches.items[i].id, false) != 0) {
            sigmund_free_alias_match_list(&matches);
            return -1;
        }
    }
    sigmund_free_alias_match_list(&matches);
    return 1;
}

static int collect_public_alias_matches(const struct sigmund_store *store,
                                        const char *alias,
                                        struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!sigmund_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = sigmund_alias_exists_in_store(store, alias);
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!sigmund_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!sigmund_valid_id(id)) {
            continue;
        }
        struct sigmund_public_index pi;
        if (sigmund_load_public_index_by_id(store, id, &pi) != 0 ||
            !pi.has_alias || strcmp(pi.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        struct sigmund_run_record pseudo;
        memset(&pseudo, 0, sizeof(pseudo));
        snprintf(pseudo.id, sizeof(pseudo.id), "%s", id);
        if (append_alias_match(list, &pseudo, STATE_UNKNOWN, pi.started_at[0] ? pi.started_at : "-") != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int append_public_alias_elevation_target(struct sigmund_resolved_target **targets,
                                                int *count,
                                                const struct sigmund_store *system_store,
                                                const char *alias,
                                                const char *command,
                                                bool all) {
    char hash[PROFILE_HASH_STR_LEN];
    if (sigmund_alias_lookup_hash(system_store, alias, hash) != 0) {
        if (!public_alias_visible(system_store, alias)) {
            return 0;
        }
        return 1;
    }
    struct alias_match_list matches;
    if (collect_public_alias_matches(system_store, alias, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        sigmund_free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count == 0) {
        sigmund_free_alias_match_list(&matches);
        return 1;
    }
    if (matches.count > 1) {
        if (!all || !sigmund_command_all_allowed(command)) {
            report_alias_ambiguity(command, alias, &matches);
            sigmund_free_alias_match_list(&matches);
            return -2;
        }
        int rc = append_capability_target(targets, count, system_store, "ffffffff", alias, hash) == 0 ? 1 : -1;
        sigmund_free_alias_match_list(&matches);
        return rc;
    }
    int rc = append_capability_target(targets, count, system_store, matches.items[0].id, alias, hash) == 0 ? 1 : -1;
    sigmund_free_alias_match_list(&matches);
    return rc;
}

int sigmund_resolve_target(const struct sigmund_invocation *inv,
                          const struct sigmund_store *current_user_store,
                          const struct sigmund_store *system_store,
                          const char *token,
                          struct sigmund_resolved_target *out) {
    memset(out, 0, sizeof(*out));
    out->scope = RESOLVE_NOT_FOUND;

    const char *id = NULL;
    enum id_token_scope token_scope = sigmund_parse_id_token(token, &id);
    if (token_scope == ID_TOKEN_INVALID || !sigmund_valid_target_atom(id)) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
        out->scope = RESOLVE_ERROR;
        return -1;
    }

    if (inv->euid_root) {
        if (token_scope == ID_TOKEN_USER) {
            struct sigmund_store user_store;
            if (sigmund_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", id);
                out->scope = RESOLVE_ERROR;
                return -1;
            }
            char resolved[16];
            if (resolve_user_store_id(&user_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_USER_LOCAL, &user_store, resolved, false);
                return 0;
            }
            return 0;
        }
        if (token_scope == ID_TOKEN_SYSTEM) {
            char resolved[16];
            if (resolve_system_private_id(system_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false);
                return 0;
            }
            return 0;
        }

        char root_resolved[16] = {0};
        char user_resolved[16] = {0};
        bool root_match = resolve_system_private_id(system_store, id, root_resolved, sizeof(root_resolved)) == 0;
        bool user_match = false;
        struct sigmund_store user_store;
        if (inv->have_sudo_user && sigmund_init_invoking_user_store(inv, &user_store) == 0) {
            user_match = resolve_user_store_id(&user_store, id, user_resolved, sizeof(user_resolved)) == 0;
        }
        if (root_match) {
            (void)user_match;
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, root_resolved, false);
            return 0;
        }
        if (user_match) {
            fill_target(out, RESOLVE_USER_LOCAL, &user_store, user_resolved, false);
            return 0;
        }
        return 0;
    }

    if (token_scope == ID_TOKEN_USER) {
        char resolved[16];
        if (resolve_user_store_id(current_user_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_USER_LOCAL, current_user_store, resolved, false);
            return 0;
        }
        return 0;
    }
    if (token_scope == ID_TOKEN_SYSTEM) {
        char resolved[16];
        if (resolve_system_public_id(system_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true);
            return 0;
        }
        return 0;
    }

    char user_resolved[16];
    if (resolve_user_store_id(current_user_store, id, user_resolved, sizeof(user_resolved)) == 0) {
        fill_target(out, RESOLVE_USER_LOCAL, current_user_store, user_resolved, false);
        return 0;
    }
    char system_resolved[16];
    if (resolve_system_public_id(system_store, id, system_resolved, sizeof(system_resolved)) == 0) {
        fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, system_resolved, true);
        return 0;
    }
    return 0;
}

int sigmund_report_not_found(const char *token) {
    fprintf(stderr, "sigmund: error: no run matches '%s'\n", token ? token : "");
    return 5;
}

int sigmund_resolve_action_token(const struct sigmund_invocation *inv,
                                const struct sigmund_store *current_user_store,
                                const struct sigmund_store *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct sigmund_resolved_target **targets_out,
                                int *count_out) {
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = sigmund_parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && sigmund_parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || sigmund_verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            return sigmund_report_not_found(token);
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID || (!sigmund_valid_id_prefix(atom) && !sigmund_valid_alias(atom))) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    if (!cap_token && sigmund_valid_id_prefix(atom)) {
        char resolved[16];
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct sigmund_store user_store;
                if (sigmund_init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
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
                    struct sigmund_store user_store;
                    if (sigmund_init_invoking_user_store(inv, &user_store) == 0 &&
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
                    return sigmund_report_not_found(token);
                }
            }
            if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
                if (resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true) == 0 ? 0 : 3;
                }
            }
        }
    }

    if (!sigmund_valid_alias(atom)) {
        return sigmund_report_not_found(token);
    }

    int rc = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct sigmund_store user_store;
            if (sigmund_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                return 5;
            }
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : sigmund_report_not_found(token);
        }
        if (scope == ID_TOKEN_SYSTEM) {
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : sigmund_report_not_found(token);
        }

        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (inv->have_sudo_user) {
            struct sigmund_store user_store;
            if (sigmund_init_invoking_user_store(inv, &user_store) == 0) {
                rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
                if (rc == 1) return 0;
                if (rc == -2) return 6;
                if (rc < 0) return 3;
            }
        }
        return sigmund_report_not_found(token);
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (scope == ID_TOKEN_USER) {
            return sigmund_report_not_found(token);
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        rc = append_public_alias_elevation_target(targets_out, count_out, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
    }
    return sigmund_report_not_found(token);
}

int sigmund_elevate_with_sudo_targets(const char *program,
                               const char *command,
                               char **original_tokens,
                               const struct sigmund_resolved_target *targets,
                               int ntargets,
                               bool all,
                               bool print_cmd) {
    int target_argc = 0;
    bool has_capability = false;
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            target_argc += 3;
            has_capability = true;
        } else {
            target_argc += 1;
        }
    }
    int canonical_argc = 1 + ((!has_capability && all) ? 1 : 0) + (print_cmd ? 1 : 0) + target_argc;
    char **canon = calloc((size_t)canonical_argc, sizeof(char *));
    char **tokens = calloc((size_t)ntargets, sizeof(char *));
    if (!canon || !tokens) {
        free(canon);
        free(tokens);
        return 3;
    }

    int n = 0;
    canon[n++] = (char *)command;
    if (!has_capability && all) {
        canon[n++] = "--all";
    }
    if (print_cmd) {
        canon[n++] = "--print";
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            canon[n++] = (char *)targets[i].id;
            canon[n++] = (char *)targets[i].cap_alias;
            canon[n++] = (char *)targets[i].cap_hash;
            continue;
        }
        const char *orig_id = NULL;
        enum id_token_scope orig_scope = sigmund_parse_id_token(original_tokens ? original_tokens[i] : NULL, &orig_id);
        const char *prefix = "";
        if (targets[i].scope == RESOLVE_USER_LOCAL) {
            prefix = "user:";
        } else if (targets[i].scope == RESOLVE_SYSTEM_MANAGED &&
                   (orig_scope == ID_TOKEN_SYSTEM || targets[i].needs_elevation)) {
            prefix = "system:";
        }
        size_t need = strlen(prefix) + strlen(targets[i].id) + 1;
        tokens[i] = malloc(need);
        if (!tokens[i]) {
            for (int j = 0; j < i; j++) free(tokens[j]);
            free(tokens);
            free(canon);
            return 3;
        }
        snprintf(tokens[i], need, "%s%s", prefix, targets[i].id);
        canon[n++] = tokens[i];
    }

    int rc = sigmund_elevate_with_sudo_canonical(program, canonical_argc, canon);
    for (int i = 0; i < ntargets; i++) free(tokens[i]);
    free(tokens);
    free(canon);
    return rc;
}

int sigmund_maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out) {
    if (!sigmund_command_accepts_target_tokens(command) || argc <= 0) {
        return 0;
    }
    struct sigmund_store system_store;
    if (sigmund_init_system_store(&system_store) != 0) {
        return 0;
    }
    char **canon = calloc((size_t)argc * 3 + 3, sizeof(char *));
    char **owned_tokens = calloc((size_t)argc * 3, sizeof(char *));
    if (!canon || !owned_tokens) {
        free(canon);
        free(owned_tokens);
        *rc_out = 3;
        return 1;
    }
    bool changed = false;
    int n = 0;
    canon[n++] = (char *)command;
    for (int i = 0; i < argc; i++) {
        const char *token = argv[i];
        const char *atom = NULL;
        enum id_token_scope scope = sigmund_parse_id_token(token, &atom);
        if (!strcmp(command, "prune") && strcmp(token, "all") == 0) {
            canon[n++] = argv[i];
            continue;
        }
        if ((scope == ID_TOKEN_PLAIN || scope == ID_TOKEN_SYSTEM) && atom && sigmund_valid_alias(atom)) {
            char hash[PROFILE_HASH_STR_LEN];
            if (sigmund_alias_lookup_hash(&system_store, atom, hash) == 0) {
                struct alias_match_list matches;
                if (collect_public_alias_matches(&system_store, atom, &matches) != 0) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                const char *selector = NULL;
                char selector_buf[16];
                if (matches.count == 0) {
                    sigmund_free_alias_match_list(&matches);
                    *rc_out = 0;
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    return 1;
                }
                if (matches.count > 1) {
                    if (!all || !sigmund_command_all_allowed(command)) {
                        report_alias_ambiguity(command, atom, &matches);
                        sigmund_free_alias_match_list(&matches);
                        *rc_out = 6;
                        for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                        free(owned_tokens);
                        free(canon);
                        return 1;
                    }
                    selector = "ffffffff";
                } else {
                    snprintf(selector_buf, sizeof(selector_buf), "%s", matches.items[0].id);
                    selector = selector_buf;
                }
                size_t slot = (size_t)i * 3;
                owned_tokens[slot] = strdup(selector);
                owned_tokens[slot + 1] = strdup(atom);
                owned_tokens[slot + 2] = strdup(hash);
                sigmund_free_alias_match_list(&matches);
                if (!owned_tokens[slot] || !owned_tokens[slot + 1] || !owned_tokens[slot + 2]) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                canon[n++] = owned_tokens[slot];
                canon[n++] = owned_tokens[slot + 1];
                canon[n++] = owned_tokens[slot + 2];
                changed = true;
                continue;
            }
        }
        canon[n++] = argv[i];
    }
    if (!changed) {
        free(owned_tokens);
        free(canon);
        return 0;
    }
    *rc_out = sigmund_elevate_with_sudo_canonical(program, n, canon);
    for (int i = 0; i < argc * 3; i++) {
        free(owned_tokens[i]);
    }
    free(owned_tokens);
    free(canon);
    return 1;
}
