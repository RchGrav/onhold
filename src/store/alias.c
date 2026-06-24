#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/store.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"

static int parse_alias_recipe_object(const char *j, struct sigmund_alias *entry);
static int write_aliases_atomic(const struct sigmund_store *store, const struct sigmund_alias *entries, size_t count);

void sigmund_free_aliases(struct sigmund_alias *entries, size_t count) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        sigmund_free_argv_alloc(entries[i].argv, entries[i].argc);
    }
    free(entries);
}

static int parse_alias_recipe_object(const char *j, struct sigmund_alias *entry) {
    if (sigmund_json_get_str(j, "bin", entry->binary_path, sizeof(entry->binary_path)) != 0 &&
        sigmund_json_get_str(j, "binary_path", entry->binary_path, sizeof(entry->binary_path)) != 0) {
        return -1;
    }
    if (entry->binary_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (sigmund_json_get_args_alloc(j, &entry->argv, &entry->argc) != 0 &&
        sigmund_json_get_argv_alloc(j, &entry->argv, &entry->argc) != 0) {
        return -1;
    }
    entry->has_recipe = true;
    return 0;
}

int sigmund_load_aliases(const struct sigmund_store *store, struct sigmund_alias **entries_out, size_t *count_out) {
    *entries_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (sigmund_read_owned_file_no_symlink(store->alias_path, &j) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    const char *p = sigmund_skip_ws(j);
    if (*p != '{') {
        free(j);
        errno = EINVAL;
        return -1;
    }
    p++;
    size_t cap = 0, count = 0;
    struct sigmund_alias *entries = NULL;
    while (1) {
        p = sigmund_skip_ws(p);
        if (*p == '}') {
            break;
        }
        char name[ALIAS_MAX_LEN + 1], hash[PROFILE_HASH_STR_LEN];
        if (sigmund_parse_json_string(p, name, sizeof(name), &p) != 0 || !sigmund_valid_alias(name)) {
            free(j);
            sigmund_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = sigmund_skip_ws(p);
        if (*p != ':') {
            free(j);
            sigmund_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = sigmund_skip_ws(p + 1);
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct sigmund_alias *next = realloc(entries, next_cap * sizeof(*entries));
            if (!next) {
                free(j);
                sigmund_free_aliases(entries, count);
                return -1;
            }
            entries = next;
            cap = next_cap;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        const char *value = p;
        if (*value == '"') {
            if (sigmund_parse_json_string(value, hash, sizeof(hash), NULL) != 0 || !sigmund_valid_profile_hash(hash)) {
                free(j);
                sigmund_free_aliases(entries, count);
                errno = EINVAL;
                return -1;
            }
            snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
            entries[count].has_hash = true;
        } else if (*value == '{') {
            if (parse_alias_recipe_object(value, &entries[count]) != 0) {
                free(j);
                sigmund_free_aliases(entries, count + 1);
                return -1;
            }
        } else {
            free(j);
            sigmund_free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = value;
        if (sigmund_skip_json_value(&p) != 0) {
            free(j);
            sigmund_free_aliases(entries, count + 1);
            return -1;
        }
        count++;
        p = sigmund_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        sigmund_free_aliases(entries, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int write_aliases_atomic(const struct sigmund_store *store, const struct sigmund_alias *entries, size_t count) {
    const char *dir = store->kind == STORE_SYSTEM_MANAGED ? store->public_dir : store->base;
    char tmp[SIGMUND_PATH_MAX];
    mode_t mode = store->kind == STORE_SYSTEM_MANAGED ? 0644 : 0600;
    int fd = sigmund_open_unique_temp(dir, "aliases", mode, tmp, sizeof(tmp));
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
        sigmund_json_escape(f, entries[i].name);
        if (store->kind == STORE_SYSTEM_MANAGED) {
            if (!entries[i].has_hash || !sigmund_valid_profile_hash(entries[i].hash)) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": \"");
            sigmund_json_escape(f, entries[i].hash);
            fprintf(f, "\"%s\n", i + 1 == count ? "" : ",");
        } else {
            if (!entries[i].has_recipe || entries[i].binary_path[0] != '/' || entries[i].argc <= 0 || !entries[i].argv) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": {\"bin\": \"");
            sigmund_json_escape(f, entries[i].binary_path);
            fprintf(f, "\", \"args\": ");
            sigmund_write_json_argv(f, entries[i].argc, entries[i].argv);
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
    (void)sigmund_fsync_dir_path(dir);
    return 0;
}

int sigmund_alias_lookup_hash(const struct sigmund_store *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]) {
    if (!sigmund_valid_alias(alias)) {
        return -1;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
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
    sigmund_free_aliases(entries, count);
    return rc;
}

int sigmund_alias_upsert_hash(const struct sigmund_store *store, const char *alias, const char *hash) {
    if (!sigmund_valid_alias(alias) || !sigmund_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            snprintf(entries[i].hash, sizeof(entries[i].hash), "%s", hash);
            entries[i].has_hash = true;
            int rc = write_aliases_atomic(store, entries, count);
            sigmund_free_aliases(entries, count);
            return rc;
        }
    }
    struct sigmund_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        sigmund_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
    entries[count].has_hash = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    sigmund_free_aliases(entries, count);
    return rc;
}

int sigmund_alias_lookup_recipe(const struct sigmund_store *store, const char *alias, struct sigmund_profile *recipe) {
    memset(recipe, 0, sizeof(*recipe));
    if (!sigmund_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        if (entries[i].has_recipe) {
            if (sigmund_checked_snprintf(recipe->binary_path, sizeof(recipe->binary_path), "%s", entries[i].binary_path) == 0 &&
                sigmund_copy_argv(&recipe->argv, entries[i].argc, entries[i].argv) == 0) {
                recipe->argc = entries[i].argc;
                rc = 0;
            }
            break;
        }
        if (entries[i].has_hash && sigmund_load_profile_by_hash(store, entries[i].hash, recipe) == 0) {
            rc = 0;
            break;
        }
    }
    sigmund_free_aliases(entries, count);
    return rc;
}

int sigmund_alias_upsert_recipe(const struct sigmund_store *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv) {
    if (!sigmund_valid_alias(alias) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            sigmund_free_argv_alloc(entries[i].argv, entries[i].argc);
            entries[i].argv = NULL;
            entries[i].argc = 0;
            if (sigmund_checked_snprintf(entries[i].binary_path, sizeof(entries[i].binary_path), "%s", binary_path) != 0 ||
                sigmund_copy_argv(&entries[i].argv, argc, argv) != 0) {
                sigmund_free_aliases(entries, count);
                return -1;
            }
            entries[i].argc = argc;
            entries[i].has_recipe = true;
            entries[i].has_hash = false;
            entries[i].hash[0] = '\0';
            int rc = write_aliases_atomic(store, entries, count);
            sigmund_free_aliases(entries, count);
            return rc;
        }
    }
    struct sigmund_alias *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        sigmund_free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    if (sigmund_checked_snprintf(entries[count].binary_path, sizeof(entries[count].binary_path), "%s", binary_path) != 0 ||
        sigmund_copy_argv(&entries[count].argv, argc, argv) != 0) {
        sigmund_free_aliases(entries, count + 1);
        return -1;
    }
    entries[count].argc = argc;
    entries[count].has_recipe = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    sigmund_free_aliases(entries, count);
    return rc;
}

int sigmund_alias_delete(const struct sigmund_store *store, const char *alias, bool *deleted) {
    if (deleted) {
        *deleted = false;
    }
    if (!sigmund_valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        sigmund_free_argv_alloc(entries[i].argv, entries[i].argc);
        for (size_t j = i + 1; j < count; j++) {
            entries[j - 1] = entries[j];
        }
        memset(&entries[count - 1], 0, sizeof(entries[count - 1]));
        count--;
        int rc = write_aliases_atomic(store, entries, count);
        if (deleted && rc == 0) {
            *deleted = true;
        }
        sigmund_free_aliases(entries, count);
        return rc;
    }
    sigmund_free_aliases(entries, count);
    return 0;
}

int sigmund_alias_rename(const struct sigmund_store *store, const char *old_alias, const char *new_alias) {
    if (!sigmund_valid_alias(old_alias) || !sigmund_valid_alias(new_alias)) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(old_alias, new_alias) == 0) {
        return 0;
    }
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    ssize_t old_idx = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, new_alias) == 0) {
            sigmund_free_aliases(entries, count);
            errno = EEXIST;
            return -1;
        }
        if (strcmp(entries[i].name, old_alias) == 0) {
            old_idx = (ssize_t)i;
        }
    }
    if (old_idx < 0) {
        sigmund_free_aliases(entries, count);
        errno = ENOENT;
        return -1;
    }
    if (sigmund_checked_snprintf(entries[old_idx].name, sizeof(entries[old_idx].name), "%s", new_alias) != 0) {
        sigmund_free_aliases(entries, count);
        return -1;
    }
    int rc = write_aliases_atomic(store, entries, count);
    sigmund_free_aliases(entries, count);
    return rc;
}

int sigmund_parse_alias_cap_atom(const char *atom,
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
    if (!sigmund_valid_alias(alias_tmp) || !sigmund_valid_profile_hash(sep + 1)) {
        return -1;
    }
    snprintf(alias, ALIAS_MAX_LEN + 1, "%s", alias_tmp);
    snprintf(hash, PROFILE_HASH_STR_LEN, "%s", sep + 1);
    return 0;
}

int sigmund_verify_system_alias_cap(const struct sigmund_store *system_store,
                                   const char *alias,
                                   const char *hash) {
    char current[PROFILE_HASH_STR_LEN];
    struct sigmund_profile p;
    if (!sigmund_valid_alias(alias) || !sigmund_valid_profile_hash(hash) ||
        sigmund_alias_lookup_hash(system_store, alias, current) != 0 ||
        strcmp(current, hash) != 0 ||
        sigmund_load_profile_by_hash(system_store, hash, &p) != 0) {
        return -1;
    }
    sigmund_free_profile(&p);
    return 0;
}

bool sigmund_alias_exists_in_store(const struct sigmund_store *store, const char *alias) {
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (!sigmund_valid_alias(alias) || sigmund_load_aliases(store, &entries, &count) != 0) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            found = true;
            break;
        }
    }
    sigmund_free_aliases(entries, count);
    return found;
}
