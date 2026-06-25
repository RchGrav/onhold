#include "hold/config.h"
#include "hold/types.h"
#include "hold/store.h"
#include "hold/core.h"
#include "hold/platform.h"

static int parse_alias_recipe_object(const char *j, struct hold_alias *entry);
static int write_aliases_atomic(const struct hold_store *store, const struct hold_alias *entries, size_t count);

void hold_free_aliases(struct hold_alias *entries, size_t count) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        hold_free_argv_alloc(entries[i].argv, entries[i].argc);
        hold_free_argv_alloc(entries[i].env, entries[i].envc);
        hold_free_argv_alloc(entries[i].ports, entries[i].portc);
        hold_free_argv_alloc(entries[i].volumes, entries[i].volumec);
    }
    free(entries);
}

static int parse_alias_recipe_object(const char *j, struct hold_alias *entry) {
    if (hold_json_get_str(j, "bin", entry->binary_path, sizeof(entry->binary_path)) != 0 &&
        hold_json_get_str(j, "binary_path", entry->binary_path, sizeof(entry->binary_path)) != 0) {
        return -1;
    }
    if (entry->binary_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (hold_json_get_args_alloc(j, &entry->argv, &entry->argc) != 0 &&
        hold_json_get_argv_alloc(j, &entry->argv, &entry->argc) != 0) {
        return -1;
    }
    if (hold_json_get_env_alloc(j, &entry->env, &entry->envc) != 0) {
        entry->env = NULL;
        entry->envc = 0;
    }
    if (hold_json_get_ports_alloc(j, &entry->ports, &entry->portc) != 0) {
        entry->ports = NULL;
        entry->portc = 0;
    }
    if (hold_json_get_volumes_alloc(j, &entry->volumes, &entry->volumec) != 0) {
        entry->volumes = NULL;
        entry->volumec = 0;
    }
    (void)hold_json_get_bool(j, "interactive", &entry->mode_interactive);
    (void)hold_json_get_bool(j, "tty", &entry->mode_tty);
    (void)hold_json_get_bool(j, "detach", &entry->mode_detach);
    (void)hold_json_get_bool(j, "multi", &entry->allow_multi);
    if (hold_json_get_str(j, "restart", entry->restart_policy, sizeof(entry->restart_policy)) == 0 &&
        entry->restart_policy[0] && strcmp(entry->restart_policy, "no") != 0) {
        entry->has_restart_policy = true;
    }
    int64_t restart_delay_tmp = 0;
    if (hold_json_get_i64(j, "restart_delay_seconds", &restart_delay_tmp) == 0 && restart_delay_tmp >= 0 && restart_delay_tmp <= INT_MAX) {
        entry->restart_delay_seconds = (int)restart_delay_tmp;
        entry->has_restart_delay = entry->has_restart_policy;
    }
    const char *mode = NULL;
    if (hold_json_find_key(j, "mode", &mode) == 0 && *mode == '{') {
        const char *end = mode;
        if (hold_skip_json_value(&end) == 0 && end > mode) {
            size_t len = (size_t)(end - mode);
            char *copy = malloc(len + 1);
            if (!copy) return -1;
            memcpy(copy, mode, len);
            copy[len] = '\0';
            (void)hold_json_get_bool(copy, "interactive", &entry->mode_interactive);
            (void)hold_json_get_bool(copy, "tty", &entry->mode_tty);
            (void)hold_json_get_bool(copy, "detach", &entry->mode_detach);
            (void)hold_json_get_bool(copy, "multi", &entry->allow_multi);
            free(copy);
        }
    }
    entry->has_recipe = true;
    return 0;
}

int hold_load_aliases(const struct hold_store *store, struct hold_alias **entries_out, size_t *count_out) {
    *entries_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(store->alias_path, &j) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    const char *p = hold_skip_ws(j);
    if (*p != '{') {
        free(j);
        errno = EINVAL;
        return -1;
    }
    p++;
    size_t cap = 0, count = 0;
    struct hold_alias *entries = NULL;
    while (1) {
        p = hold_skip_ws(p);
        if (*p == '}') {
            break;
        }
        char name[ALIAS_MAX_LEN + 1], hash[PROFILE_HASH_STR_LEN];
        if (hold_parse_json_string(p, name, sizeof(name), &p) != 0 || !hold_valid_alias(name)) {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p);
        if (*p != ':') {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = hold_skip_ws(p + 1);
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct hold_alias *next = realloc(entries, next_cap * sizeof(*entries));
            if (!next) {
                free(j);
                hold_free_aliases(entries, count);
                return -1;
            }
            entries = next;
            cap = next_cap;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        const char *value = p;
        if (*value == '"') {
            if (hold_parse_json_string(value, hash, sizeof(hash), NULL) != 0 || !hold_valid_profile_hash(hash)) {
                free(j);
                hold_free_aliases(entries, count);
                errno = EINVAL;
                return -1;
            }
            snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
            entries[count].has_hash = true;
        } else if (*value == '{') {
            if (parse_alias_recipe_object(value, &entries[count]) != 0) {
                free(j);
                hold_free_aliases(entries, count + 1);
                return -1;
            }
        } else {
            free(j);
            hold_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = value;
        if (hold_skip_json_value(&p) != 0) {
            free(j);
            hold_free_aliases(entries, count + 1);
            return -1;
        }
        count++;
        p = hold_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        hold_free_aliases(entries, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int write_aliases_atomic(const struct hold_store *store, const struct hold_alias *entries, size_t count) {
    const char *dir = store->kind == STORE_SYSTEM_MANAGED ? store->public_dir : store->base;
    char tmp[HOLD_PATH_MAX];
    mode_t mode = store->kind == STORE_SYSTEM_MANAGED ? 0644 : 0600;
    int fd = hold_open_unique_temp(dir, "aliases", mode, tmp, sizeof(tmp));
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (store->kind == STORE_SYSTEM_MANAGED && geteuid() == 0 && fchown(fd, 0, 0) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "{\n");
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "  \"");
        hold_json_escape(f, entries[i].name);
        if (store->kind == STORE_SYSTEM_MANAGED) {
            if (!entries[i].has_hash || !hold_valid_profile_hash(entries[i].hash)) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": \"");
            hold_json_escape(f, entries[i].hash);
            fprintf(f, "\"%s\n", i + 1 == count ? "" : ",");
        } else {
            if (!entries[i].has_recipe || entries[i].binary_path[0] != '/' || entries[i].argc <= 0 || !entries[i].argv) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": {\"bin\": \"");
            hold_json_escape(f, entries[i].binary_path);
            fprintf(f, "\", \"args\": ");
            hold_write_json_argv(f, entries[i].argc, entries[i].argv);
            if (entries[i].envc > 0 && entries[i].env) {
                fprintf(f, ", \"env\": ");
                hold_write_json_argv(f, entries[i].envc, entries[i].env);
            }
            if (entries[i].portc > 0 && entries[i].ports) {
                fprintf(f, ", \"ports\": ");
                hold_write_json_argv(f, entries[i].portc, entries[i].ports);
            }
            if (entries[i].volumec > 0 && entries[i].volumes) {
                fprintf(f, ", \"volumes\": ");
                hold_write_json_argv(f, entries[i].volumec, entries[i].volumes);
            }
            if (entries[i].mode_interactive || entries[i].mode_tty || entries[i].mode_detach || entries[i].allow_multi) {
                fprintf(f, ", \"mode\": {");
                bool wrote_mode = false;
                if (entries[i].mode_interactive) {
                    fprintf(f, "\"interactive\": true");
                    wrote_mode = true;
                }
                if (entries[i].mode_tty) {
                    fprintf(f, "%s\"tty\": true", wrote_mode ? ", " : "");
                    wrote_mode = true;
                }
                if (entries[i].mode_detach) {
                    fprintf(f, "%s\"detach\": true", wrote_mode ? ", " : "");
                    wrote_mode = true;
                }
                if (entries[i].allow_multi) {
                    fprintf(f, "%s\"multi\": true", wrote_mode ? ", " : "");
                }
                fprintf(f, "}");
            }
            if (entries[i].has_restart_policy && entries[i].restart_policy[0]) {
                fprintf(f, ", \"restart\": \"");
                hold_json_escape(f, entries[i].restart_policy);
                fprintf(f, "\"");
            }
            if (entries[i].has_restart_delay) {
                fprintf(f, ", \"restart_delay_seconds\": %d", entries[i].restart_delay_seconds);
            }
            fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
        }
    }
    fprintf(f, "}\n");
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, store->alias_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

int hold_alias_lookup_hash(const struct hold_store *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]) {
    if (!hold_valid_alias(alias)) {
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0 && entries[i].has_hash) {
            snprintf(hash, PROFILE_HASH_STR_LEN, "%s", entries[i].hash);
            rc = 0;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_upsert_hash(const struct hold_store *store, const char *alias, const char *hash) {
    if (!hold_valid_alias(alias) || !hold_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            snprintf(entries[i].hash, sizeof(entries[i].hash), "%s", hash);
            entries[i].has_hash = true;
            int rc = write_aliases_atomic(store, entries, count);
            hold_free_aliases(entries, count);
            return rc;
        }
    }
    struct hold_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        hold_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
    entries[count].has_hash = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_lookup_recipe(const struct hold_store *store, const char *alias, struct hold_profile *recipe) {
    memset(recipe, 0, sizeof(*recipe));
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        if (entries[i].has_recipe) {
            if (hold_checked_snprintf(recipe->binary_path, sizeof(recipe->binary_path), "%s", entries[i].binary_path) == 0 &&
                hold_copy_argv(&recipe->argv, entries[i].argc, entries[i].argv) == 0 &&
                (entries[i].envc == 0 || hold_copy_argv(&recipe->env, entries[i].envc, entries[i].env) == 0) &&
                (entries[i].portc == 0 || hold_copy_argv(&recipe->ports, entries[i].portc, entries[i].ports) == 0) &&
                (entries[i].volumec == 0 || hold_copy_argv(&recipe->volumes, entries[i].volumec, entries[i].volumes) == 0)) {
                recipe->argc = entries[i].argc;
                recipe->envc = entries[i].envc;
                recipe->portc = entries[i].portc;
                recipe->volumec = entries[i].volumec;
                recipe->mode_interactive = entries[i].mode_interactive;
                recipe->mode_tty = entries[i].mode_tty;
                recipe->mode_detach = entries[i].mode_detach;
                recipe->allow_multi = entries[i].allow_multi;
                if (entries[i].has_restart_policy) {
                    snprintf(recipe->restart_policy, sizeof(recipe->restart_policy), "%s", entries[i].restart_policy);
                    recipe->has_restart_policy = true;
                }
                recipe->restart_delay_seconds = entries[i].restart_delay_seconds;
                recipe->has_restart_delay = entries[i].has_restart_delay;
                rc = 0;
            }
            break;
        }
        if (entries[i].has_hash && hold_load_profile_by_hash(store, entries[i].hash, recipe) == 0) {
            rc = 0;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_upsert_recipe(const struct hold_store *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv) {
    return hold_alias_upsert_recipe_env(store, alias, binary_path, argc, argv, 0, NULL);
}

int hold_alias_upsert_recipe_env(const struct hold_store *store,
                                   const char *alias,
                                   const char *binary_path,
                                   int argc,
                                   char **argv,
                                   int envc,
                                   char **env) {
    return hold_alias_upsert_recipe_full(store, alias, binary_path, argc, argv, envc, env, 0, NULL, 0, NULL, false, false, false, false, NULL, 0);
}

int hold_alias_upsert_recipe_full(const struct hold_store *store,
                                   const char *alias,
                                   const char *binary_path,
                                   int argc,
                                   char **argv,
                                   int envc,
                                   char **env,
                                   int portc,
                                   char **ports,
                                   int volumec,
                                   char **volumes,
                                   bool mode_interactive,
                                   bool mode_tty,
                                   bool mode_detach,
                                   bool allow_multi,
                                   const char *restart_policy,
                                   int restart_delay_seconds) {
    if (!hold_valid_alias(alias) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv ||
        envc < 0 || (envc > 0 && !env) ||
        portc < 0 || (portc > 0 && !ports) ||
        volumec < 0 || (volumec > 0 && !volumes) ||
        restart_delay_seconds < 0) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            hold_free_argv_alloc(entries[i].argv, entries[i].argc);
            hold_free_argv_alloc(entries[i].env, entries[i].envc);
            hold_free_argv_alloc(entries[i].ports, entries[i].portc);
            hold_free_argv_alloc(entries[i].volumes, entries[i].volumec);
            entries[i].argv = NULL;
            entries[i].argc = 0;
            entries[i].env = NULL;
            entries[i].envc = 0;
            entries[i].ports = NULL;
            entries[i].portc = 0;
            entries[i].volumes = NULL;
            entries[i].volumec = 0;
            if (hold_checked_snprintf(entries[i].binary_path, sizeof(entries[i].binary_path), "%s", binary_path) != 0 ||
                hold_copy_argv(&entries[i].argv, argc, argv) != 0 ||
                (envc > 0 && hold_copy_argv(&entries[i].env, envc, env) != 0) ||
                (portc > 0 && hold_copy_argv(&entries[i].ports, portc, ports) != 0) ||
                (volumec > 0 && hold_copy_argv(&entries[i].volumes, volumec, volumes) != 0)) {
                hold_free_aliases(entries, count);
                return -1;
            }
            entries[i].argc = argc;
            entries[i].envc = envc;
            entries[i].portc = portc;
            entries[i].volumec = volumec;
            entries[i].mode_interactive = mode_interactive;
            entries[i].mode_tty = mode_tty;
            entries[i].mode_detach = mode_detach;
            entries[i].allow_multi = allow_multi;
            entries[i].restart_policy[0] = '\0';
            entries[i].has_restart_policy = false;
            if (restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0) {
                snprintf(entries[i].restart_policy, sizeof(entries[i].restart_policy), "%s", restart_policy);
                entries[i].has_restart_policy = true;
            }
            entries[i].restart_delay_seconds = entries[i].has_restart_policy ? restart_delay_seconds : 0;
            entries[i].has_restart_delay = entries[i].has_restart_policy && restart_delay_seconds > 0;
            entries[i].has_recipe = true;
            entries[i].has_hash = false;
            entries[i].hash[0] = '\0';
            int rc = write_aliases_atomic(store, entries, count);
            hold_free_aliases(entries, count);
            return rc;
        }
    }
    struct hold_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        hold_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    if (hold_checked_snprintf(entries[count].binary_path, sizeof(entries[count].binary_path), "%s", binary_path) != 0 ||
        hold_copy_argv(&entries[count].argv, argc, argv) != 0 ||
        (envc > 0 && hold_copy_argv(&entries[count].env, envc, env) != 0) ||
        (portc > 0 && hold_copy_argv(&entries[count].ports, portc, ports) != 0) ||
        (volumec > 0 && hold_copy_argv(&entries[count].volumes, volumec, volumes) != 0)) {
        hold_free_aliases(entries, count + 1);
        return -1;
    }
    entries[count].argc = argc;
    entries[count].envc = envc;
    entries[count].portc = portc;
    entries[count].volumec = volumec;
    entries[count].mode_interactive = mode_interactive;
    entries[count].mode_tty = mode_tty;
    entries[count].mode_detach = mode_detach;
    entries[count].allow_multi = allow_multi;
    if (restart_policy && *restart_policy && strcmp(restart_policy, "no") != 0) {
        snprintf(entries[count].restart_policy, sizeof(entries[count].restart_policy), "%s", restart_policy);
        entries[count].has_restart_policy = true;
    }
    entries[count].restart_delay_seconds = entries[count].has_restart_policy ? restart_delay_seconds : 0;
    entries[count].has_restart_delay = entries[count].has_restart_policy && restart_delay_seconds > 0;
    entries[count].has_recipe = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_alias_delete(const struct hold_store *store, const char *alias, bool *deleted) {
    if (deleted) {
        *deleted = false;
    }
    if (!hold_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        hold_free_argv_alloc(entries[i].argv, entries[i].argc);
        hold_free_argv_alloc(entries[i].env, entries[i].envc);
        hold_free_argv_alloc(entries[i].ports, entries[i].portc);
        hold_free_argv_alloc(entries[i].volumes, entries[i].volumec);
        for (size_t j = i + 1; j < count; j++) {
            entries[j - 1] = entries[j];
        }
        memset(&entries[count - 1], 0, sizeof(entries[count - 1]));
        count--;
        int rc = write_aliases_atomic(store, entries, count);
        if (deleted && rc == 0) {
            *deleted = true;
        }
        hold_free_aliases(entries, count);
        return rc;
    }
    hold_free_aliases(entries, count);
    return 0;
}

int hold_alias_rename(const struct hold_store *store, const char *old_alias, const char *new_alias) {
    if (!hold_valid_alias(old_alias) || !hold_valid_alias(new_alias)) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(old_alias, new_alias) == 0) {
        return 0;
    }
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    ssize_t old_idx = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, new_alias) == 0) {
            hold_free_aliases(entries, count);
            errno = EEXIST;
            return -1;
        }
        if (strcmp(entries[i].name, old_alias) == 0) {
            old_idx = (ssize_t)i;
        }
    }
    if (old_idx < 0) {
        hold_free_aliases(entries, count);
        errno = ENOENT;
        return -1;
    }
    if (hold_checked_snprintf(entries[old_idx].name, sizeof(entries[old_idx].name), "%s", new_alias) != 0) {
        hold_free_aliases(entries, count);
        return -1;
    }
    int rc = write_aliases_atomic(store, entries, count);
    hold_free_aliases(entries, count);
    return rc;
}

int hold_parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]) {
    const char *sep = atom ? strchr(atom, '@') : NULL;
    if (!sep || sep == atom) {
        return -1;
    }
    size_t alias_len = (size_t)(sep - atom);
    if (alias_len > ALIAS_MAX_LEN) {
        return -1;
    }
    char alias_tmp[ALIAS_MAX_LEN + 1];
    memcpy(alias_tmp, atom, alias_len);
    alias_tmp[alias_len] = '\0';
    if (!hold_valid_alias(alias_tmp) || !hold_valid_profile_hash(sep + 1)) {
        return -1;
    }
    snprintf(alias, ALIAS_MAX_LEN + 1, "%s", alias_tmp);
    snprintf(hash, PROFILE_HASH_STR_LEN, "%s", sep + 1);
    return 0;
}

int hold_verify_system_alias_cap(const struct hold_store *system_store,
                                   const char *alias,
                                   const char *hash) {
    char current[PROFILE_HASH_STR_LEN];
    struct hold_profile p;
    if (!hold_valid_alias(alias) || !hold_valid_profile_hash(hash) ||
        hold_alias_lookup_hash(system_store, alias, current) != 0 ||
        strcmp(current, hash) != 0 ||
        hold_load_profile_by_hash(system_store, hash, &p) != 0) {
        return -1;
    }
    hold_free_profile(&p);
    return 0;
}

bool hold_alias_exists_in_store(const struct hold_store *store, const char *alias) {
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (!hold_valid_alias(alias) || hold_load_aliases(store, &entries, &count) != 0) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            found = true;
            break;
        }
    }
    hold_free_aliases(entries, count);
    return found;
}
