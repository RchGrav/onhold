#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/names/adjectives.h"
#include "hold/names/nouns.h"

/* Call-record operations split out of start.c: redial resolution, save, rename,
 * and generated call-name assignment. The start machinery (spawn, log capture,
 * restart supervisor) stays in start.c; these share only hold_perform_start_options
 * and the public runtime API. */

static int run_name_exists_in_store(const struct hold_store *store, const char *name, const char *ignore_id) {
    if (!store || !hold_valid_alias(name)) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        if (ignore_id && strcmp(file_id, ignore_id) == 0) continue;
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
        struct hold_run_record r;
        memset(&r, 0, sizeof(r));
        if (hold_load_record(path, &r) == 0 && r.has_name && strcmp(r.name, name) == 0) {
            hold_free_run_record(&r);
            closedir(d);
            return 1;
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    return 0;
}

static int find_restart_record(const struct hold_store *store, const char *token, char out[ID_STR_LEN]) {
    if (!store || !token || !*token || !out) return 0;
    bool id_like = hold_valid_id_prefix(token);
    bool name_like = hold_valid_alias(token);
    if (!id_like && !name_like) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        bool hit = false;
        if (id_like && strncmp(file_id, token, strlen(token)) == 0) {
            hit = true;
        } else if (name_like) {
            char path[HOLD_PATH_MAX];
            if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
            struct hold_run_record r;
            memset(&r, 0, sizeof(r));
            if (hold_load_record(path, &r) == 0 && r.has_name && strcmp(r.name, token) == 0) {
                hit = true;
            }
            hold_free_run_record(&r);
        }
        if (hit) {
            matches++;
            if (hold_checked_snprintf(out, ID_STR_LEN, "%s", file_id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: call '%s' is ambiguous\n", token);
        return -2;
    }
    return 1;
}

static int restart_existing_run(const struct hold_invocation *inv,
                                const struct hold_store *store,
                                bool tail,
                                bool console_mode,
                                bool auto_remove,
                                bool interactive_stdin,
                                bool explicit_session_mode,
                                const char *restart_policy,
                                int restart_delay_seconds,
                                const char *id) {
    struct hold_run_record old;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &old, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    /* Redial honors the recipe's recorded session mode: a -it recipe reattaches,
     * a -d recipe detaches (printing the id), a foreground recipe streams. An
     * explicit mode flag on the redial invocation overrides the recipe. */
    if (!explicit_session_mode) {
        tail = !old.recipe.mode_detach;
        console_mode = old.recipe.mode_tty;
        interactive_stdin = old.recipe.mode_interactive;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&old, boot_id);
    if (st == STATE_RUNNING) {
        char display[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(old.id, display);
        fprintf(stderr, "hold: error: call %s is already running\n", display);
        hold_free_run_record(&old);
        return 6;
    }
    if (!old.has_log || !old.log_path[0]) {
        fprintf(stderr, "hold: error: call %s has no retained log path\n", id);
        hold_free_run_record(&old);
        return 5;
    }
    char *j = NULL;
    char **argv = NULL;
    int argc = 0;
    int rc = 0;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0 ||
        hold_json_get_argv_alloc(j, &argv, &argc) != 0) {
        fprintf(stderr, "hold: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "hold: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    struct hold_start_options opts = {
        .tail = tail,
        .console_mode = console_mode,
        .auto_remove = auto_remove,
        .interactive_stdin = interactive_stdin,
        .argc = argc,
        .argv = argv,
        .exec_path = argv[0],
        .envc = old.recipe.envc,
        .env = old.recipe.env,
        .restart_policy = restart_policy ? restart_policy : (old.recipe.has_restart_policy ? old.recipe.restart_policy : NULL),
        .restart_delay_seconds = restart_policy ? restart_delay_seconds : old.recipe.restart_delay_seconds,
        .existing_id = old.id,
        .existing_log_path = old.log_path,
        .existing_run_name = old.has_name ? old.name : NULL,
        .existing_created_unix_ns = old.created_unix_ns,
        .existing_created_at = old.has_created_at ? old.created_at : NULL,
    };
    rc = hold_perform_start_options(inv, store, &opts);
out:
    hold_free_argv_alloc(argv, argc);
    free(j);
    hold_free_run_record(&old);
    return rc;
}

int hold_cmd_redial(const struct hold_invocation *inv,
                      const struct hold_store *user_store,
                      const struct hold_store *system_store,
                      bool tail,
                      bool console_mode,
                      bool auto_remove,
                      bool interactive_stdin,
                      bool explicit_session_mode,
                      const char *restart_policy,
                      int restart_delay_seconds,
                      const char *token,
                      bool *redialed) {
    *redialed = false;
    char restart_id[ID_STR_LEN];
    const struct hold_store *restart_store = NULL;
    int match = 0;
    if (user_store && user_store->record_dir[0]) {
        match = find_restart_record(user_store, token, restart_id);
        if (match == 1) restart_store = user_store;
    }
    if (match == 0 && system_store && system_store->record_dir[0] && inv->euid_root) {
        match = find_restart_record(system_store, token, restart_id);
        if (match == 1) restart_store = system_store;
    }
    if (match == -2) {
        *redialed = true;
        return 6;
    }
    if (match < 0) {
        *redialed = true;
        return 3;
    }
    if (!restart_store) {
        /* Not a retained call; the caller launches it as a command instead. */
        return 0;
    }
    *redialed = true;
    return restart_existing_run(inv, restart_store, tail, console_mode, auto_remove,
                                interactive_stdin, explicit_session_mode, restart_policy,
                                restart_delay_seconds, restart_id);
}

/* save and rename share one shape: resolve with the widest (inspect) intent,
 * load, mutate the protection/name state, rewrite atomically, refresh the
 * public projection for global calls, confirm on stderr. new_name==NULL
 * means save; non-NULL means rename (which also saves — naming a call
 * declares the intent to keep it). */
static int protect_record_action(const struct hold_invocation *inv,
                                 const struct hold_store *user_store,
                                 const struct hold_store *system_store,
                                 const char *target_token,
                                 const char *new_name) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", target_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(target_token);
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    if (new_name && run_name_exists_in_store(&target.store, new_name, target.id)) {
        fprintf(stderr, "hold: error: call name '%s' already exists\n", new_name);
        free(targets);
        return 5;
    }
    struct hold_run_record r;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        free(targets);
        return 5;
    }
    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    /* Saving is idempotent: an already-saved call needs no rewrite, but still
     * confirms the protected state on stderr and exits 0. */
    if (!new_name && r.saved) {
        hold_sig_note(inv, "hold: saved %s (%s)\n", display_id, r.has_name ? r.name : display_id);
        hold_free_run_record(&r);
        free(targets);
        return 0;
    }
    char *j = NULL;
    char **argv = NULL;
    int argc = 0;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0 ||
        hold_json_get_argv_alloc(j, &argv, &argc) != 0) {
        fprintf(stderr, "hold: error: failed to load call record for %s\n", target.id);
        free(j);
        hold_free_run_record(&r);
        free(targets);
        return 5;
    }
    bool newly_saved = !r.saved;
    if (new_name) {
        snprintf(r.name, sizeof(r.name), "%s", new_name);
        r.has_name = true;
    }
    r.saved = true;
    char out_path[HOLD_PATH_MAX];
    if (hold_write_record_atomic(target.store.record_dir, &r, argc, argv, out_path, sizeof(out_path)) != 0) {
        hold_die_errno(new_name ? "hold: failed to write renamed call record"
                                : "hold: failed to write saved call record");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        (void)hold_write_public_index_atomic(&target.store, &r, NULL);
    }
    if (new_name) {
        hold_sig_note(inv, "hold: renamed %s to %s%s\n", display_id, new_name,
                      newly_saved ? " (saved)" : "");
    } else {
        hold_sig_note(inv, "hold: saved %s (%s)\n", display_id, r.has_name ? r.name : display_id);
    }
    hold_free_argv_alloc(argv, argc);
    free(j);
    hold_free_run_record(&r);
    free(targets);
    return 0;
}

int hold_cmd_rename_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *target_token,
                             const char *new_name) {
    if (!hold_valid_alias(new_name)) {
        fprintf(stderr, "hold: error: invalid call name '%s'\n", new_name);
        return 5;
    }
    return protect_record_action(inv, user_store, system_store, target_token, new_name);
}

int hold_cmd_save_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *target_token) {
    return protect_record_action(inv, user_store, system_store, target_token, NULL);
}

int hold_generate_run_name_for_id(const struct hold_store *store, const char *id, const char *requested, char out[ALIAS_MAX_LEN + 1]) {
    if (requested && *requested) {
        if (!hold_valid_alias(requested)) {
            fprintf(stderr, "hold: error: invalid call name '%s'\n", requested);
            return 5;
        }
        if (run_name_exists_in_store(store, requested, NULL)) {
            fprintf(stderr, "hold: error: call name '%s' already exists\n", requested);
            return 5;
        }
        snprintf(out, ALIAS_MAX_LEN + 1, "%s", requested);
        return 0;
    }

    size_t adj_n = adjectives_count;
    size_t noun_n = nouns_count;
    size_t total = adj_n * noun_n;
    unsigned long long seed = 0;
    for (size_t i = 0; id && id[i] && i < 16; i++) {
        seed <<= 4;
        seed |= (unsigned long long)(id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
    }
    for (size_t tries = 0; tries < total; tries++) {
        unsigned long long candidate = seed + tries;
        size_t adj_idx = (size_t)(candidate % adj_n);
        size_t noun_idx = (size_t)((candidate / adj_n) % noun_n);
        if (hold_checked_snprintf(out,
                                  ALIAS_MAX_LEN + 1,
                                  "%s_%s",
                                  adjectives[adj_idx],
                                  nouns[noun_idx]) != 0) {
            return 3;
        }
        if (!run_name_exists_in_store(store, out, NULL)) return 0;
    }
    char display[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(id, display);
    return hold_checked_snprintf(out, ALIAS_MAX_LEN + 1, "run_%s", display) == 0 ? 0 : 3;
}
