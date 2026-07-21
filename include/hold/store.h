#pragma once
#ifndef HOLD_STORE_H
#define HOLD_STORE_H

#include "hold/config.h"
#include "hold/types.h"

int hold_init_user_store_from_home(const char *home, struct hold_store *store);
int hold_ensure_user_store_for_current_user(struct hold_store *store);
int hold_ensure_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);
int hold_init_system_store(struct hold_store *store);
int hold_ensure_system_store(struct hold_store *store);
int hold_write_record_atomic(const char *dir, const struct hold_run_record *r, int argc, char **argv, char *out_json_path, size_t out_n);
int hold_write_public_index_atomic(const struct hold_store *store, const struct hold_run_record *r,
                                     const char *observed_ports_csv);
int hold_mark_run_finished(const struct hold_store *store, const char *id, int status);
int hold_load_record(const char *path, struct hold_run_record *r);
void hold_free_run_record(struct hold_run_record *r);
int hold_load_public_index(const char *path, struct hold_public_index *pi);
int hold_load_public_index_by_id(const struct hold_store *store, const char *id, struct hold_public_index *pi);
int hold_load_record_by_id(const char *dir, const char *id, struct hold_run_record *r, char *path, size_t n);

#endif /* HOLD_STORE_H */
