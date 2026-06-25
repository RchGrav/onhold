#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

bool hold_valid_id(const char *id) {
    size_t len = strlen(id);
    if (len != ID_HEX_LEN) {
        return false;
    }
    if (strcmp(id, "000000000000") == 0 || strcmp(id, "ffffffffffff") == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool hold_record_json_filename_id(const char *name, char *id, size_t n) {
    if (!name || !hold_has_suffix(name, ".json")) {
        return false;
    }
    size_t len = strlen(name);
    size_t id_len = len - 5;
    if (id_len + 1 > n) {
        return false;
    }
    memcpy(id, name, id_len);
    id[id_len] = '\0';
    return hold_valid_id(id);
}

bool hold_valid_id_prefix(const char *id) {
    size_t len = strlen(id);
    if (len < 1 || len > ID_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool hold_valid_profile_hash(const char *hash) {
    if (!hash || strlen(hash) != PROFILE_HASH_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < PROFILE_HASH_HEX_LEN; i++) {
        if (!isdigit((unsigned char)hash[i]) && !(hash[i] >= 'a' && hash[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool hold_valid_alias(const char *alias) {
    if (!alias) {
        return false;
    }
    size_t len = strlen(alias);
    if (len == 0 || len > ALIAS_MAX_LEN || hold_valid_profile_hash(alias)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)alias[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool hold_valid_record(const struct hold_run_record *r) {
    return r->pid > 0 && r->pgid > 1 && r->id[0] != '\0';
}

int hold_parse_uid_env(const char *s, uid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uid_t)v;
    return 0;
}

int hold_parse_gid_env(const char *s, gid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (gid_t)v;
    return 0;
}

bool hold_valid_runid_selector(const char *sel) {
    return sel && (hold_valid_id(sel) || strcmp(sel, "000000000000") == 0 || strcmp(sel, "ffffffffffff") == 0);
}

bool hold_valid_target_atom(const char *id) {
    return hold_valid_id_prefix(id) || hold_valid_alias(id);
}
