#pragma once
#ifndef SIGMUND_STORE_H
#define SIGMUND_STORE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int chown_root_if_root(const char *path);
int init_user_store_from_home(const char *home, struct store_paths *store);
int ensure_user_store_for_current_user(struct store_paths *store);
int ensure_invoking_user_store(const struct invocation *inv, struct store_paths *store);
int init_system_store(struct store_paths *store);
int ensure_system_store(struct store_paths *store);
int gen_id_for_store(const struct store_paths *primary,
                            const struct store_paths *avoid_public_store,
                            const struct store_paths *avoid_user_store,
                            char *out,
                            size_t out_n);
void profile_hash_for_argv(const char *binary_path, int argc, char **argv, char out[PROFILE_HASH_STR_LEN]);
int write_record_atomic(const char *dir, const struct record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int write_public_index_atomic(const struct store_paths *store, const struct record *r);
void free_profile(struct profile *p);
int write_profile_atomic(const struct store_paths *store,
                                const char *hash,
                                const char *binary_path,
                                int argc,
                                char **argv);
int load_profile_by_hash(const struct store_paths *store, const char *hash, struct profile *profile);
void free_aliases(struct alias_entry *entries, size_t count);
int load_aliases(const struct store_paths *store, struct alias_entry **entries_out, size_t *count_out);
int alias_lookup_hash(const struct store_paths *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]);
int alias_upsert_hash(const struct store_paths *store, const char *alias, const char *hash);
int alias_lookup_recipe(const struct store_paths *store, const char *alias, struct profile *recipe);
int alias_upsert_recipe(const struct store_paths *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv);
int load_record(const char *path, struct record *r);
int load_public_index(const char *path, struct public_index *pi);
int load_public_index_by_id(const struct store_paths *store, const char *id, struct public_index *pi);
int load_record_by_id(const char *dir, const char *id, struct record *r, char *path, size_t n);
int parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]);
int verify_system_alias_cap(const struct store_paths *system_store,
                                   const char *alias,
                                   const char *hash);
bool alias_exists_in_store(const struct store_paths *store, const char *alias);
int profile_exists_in_store(const struct store_paths *store, const char *hash);

#endif /* SIGMUND_STORE_H */
