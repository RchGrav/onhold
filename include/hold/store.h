#pragma once
#ifndef HOLD_STORE_H
#define HOLD_STORE_H

#include "hold/config.h"
#include "hold/types.h"

int hold_chown_root_if_root(const char *path);
int hold_init_user_store_from_home(const char *home, struct hold_store *store);
int hold_ensure_user_store_for_current_user(struct hold_store *store);
int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);
int hold_init_system_store(struct hold_store *store);
int hold_ensure_system_store(struct hold_store *store);
int hold_gen_id_for_store(const struct hold_store *primary,
                            const struct hold_store *avoid_public_store,
                            const struct hold_store *avoid_user_store,
                            char *out,
                            size_t out_n);
void hold_profile_hash_for_argv(const char *binary_path, int argc, char **argv, char out[PROFILE_HASH_STR_LEN]);
int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r);
void hold_free_profile(struct hold_profile *p);
int hold_write_profile_atomic(const struct hold_store *store,
                                const char *hash,
                                const char *binary_path,
                                int argc,
                                char **argv);
int hold_write_profile_atomic_env(const struct hold_store *store,
                                    const char *hash,
                                    const char *binary_path,
                                    int argc,
                                    char **argv,
                                    int envc,
                                    char **env);
int hold_write_profile_atomic_full(const struct hold_store *store,
                                    const char *hash,
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
                                    int restart_delay_seconds);
int hold_load_profile_by_hash(const struct hold_store *store, const char *hash, struct hold_profile *profile);
void hold_free_aliases(struct hold_alias *entries, size_t count);
int hold_load_aliases(const struct hold_store *store, struct hold_alias **entries_out, size_t *count_out);
int hold_alias_lookup_hash(const struct hold_store *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]);
int hold_alias_upsert_hash(const struct hold_store *store, const char *alias, const char *hash);
int hold_alias_lookup_recipe(const struct hold_store *store, const char *alias, struct hold_profile *recipe);
int hold_alias_upsert_recipe(const struct hold_store *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv);
int hold_alias_upsert_recipe_env(const struct hold_store *store,
                                   const char *alias,
                                   const char *binary_path,
                                   int argc,
                                   char **argv,
                                   int envc,
                                   char **env);
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
                                   int restart_delay_seconds);
int hold_alias_delete(const struct hold_store *store, const char *alias, bool *deleted);
int hold_alias_rename(const struct hold_store *store, const char *old_alias, const char *new_alias);
int hold_load_record(const char *path, struct hold_run_record *r);
int hold_load_public_index(const char *path, struct hold_public_index *pi);
int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi);
int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n);
int hold_parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]);
int hold_verify_system_alias_cap(const struct hold_store *system_store,
                                   const char *alias,
                                   const char *hash);
bool hold_alias_exists_in_store(const struct hold_store *store, const char *alias);
int hold_profile_exists_in_store(const struct hold_store *store, const char *hash);

#endif /* HOLD_STORE_H */
