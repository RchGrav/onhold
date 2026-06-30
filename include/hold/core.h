#pragma once
#ifndef HOLD_CORE_H
#define HOLD_CORE_H

#include "hold/config.h"
#include "hold/types.h"

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t off;
};

void hold_die_errno(const char *msg);
void hold_sig_note(const struct hold_invocation *inv, const char *fmt, ...);
int hold_checked_snprintf(char *dst, size_t n, const char *fmt, ...);
bool hold_has_suffix(const char *s, const char *suffix);
bool hold_valid_id(const char *id);
bool hold_record_json_filename_id(const char *name, char *id, size_t n);
bool hold_valid_id_prefix(const char *id);
bool hold_valid_profile_hash(const char *hash);
bool hold_valid_alias(const char *alias);
bool hold_valid_record(const struct hold_run_record *r);
int hold_mkdir_p0700(const char *dir);
int hold_chmod_dir_no_symlink(const char *dir, mode_t mode);
int hold_chown_dir_no_symlink_if_root(const char *dir, uid_t uid, gid_t gid);
int hold_open_unique_temp(const char *dir, const char *prefix, mode_t mode, char *tmp, size_t tmp_n);
int hold_read_file_trim(const char *path, char *buf, size_t n);
bool hold_path_exists(const char *path);
void hold_sha256_init(struct sha256_ctx *c);
void hold_sha256_update(struct sha256_ctx *c, const void *data, size_t n);
void hold_sha256_final(struct sha256_ctx *c, unsigned char out[32]);
void hold_hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n);
int hold_rand_bytes(uint8_t *buf, size_t n);
int hold_mkdir_p_mode(const char *dir, mode_t mode);
int hold_parse_uid_env(const char *s, uid_t *out);
int hold_parse_gid_env(const char *s, gid_t *out);
int hold_write_all(int fd, const void *buf, size_t n);
void hold_json_escape(FILE *f, const char *s);
int hold_write_json_argv(FILE *f, int argc, char **argv);
void hold_sha256_update_nul_field(struct sha256_ctx *ctx, const char *s);
int hold_append_cmd_human(char *dst, size_t n, size_t *off, const char *arg);
int hold_format_argv_human(char *dst, size_t n, int argc, char **argv);
const char *hold_skip_ws(const char *p);
int hold_parse_json_string(const char *p, char *out, size_t n, const char **endp);
int hold_skip_json_value(const char **pp);
int hold_json_find_key(const char *j, const char *k, const char **v);
int hold_json_get_i64(const char *j, const char *k, int64_t *out);
int hold_json_get_bool(const char *j, const char *k, bool *out);
int hold_json_get_u64(const char *j, const char *k, uint64_t *out);
int hold_json_get_str(const char *j, const char *k, char *out, size_t n);
int hold_json_get_argv_display(const char *j, char *out, size_t n);
void hold_free_argv_alloc(char **argv, int argc);
int hold_json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out);
int hold_json_get_args_alloc(const char *j, char ***argv_out, int *argc_out);
int hold_json_get_env_alloc(const char *j, char ***env_out, int *envc_out);
int hold_json_get_ports_alloc(const char *j, char ***ports_out, int *portc_out);
int hold_json_get_volumes_alloc(const char *j, char ***volumes_out, int *volumec_out);
int hold_json_get_string_array_key_alloc(const char *j, const char *key, char ***items_out, int *count_out);
int hold_json_get_string_array_key_allow_empty_alloc(const char *j, const char *key, char ***items_out, int *count_out);
int hold_json_get_path_args_argv_alloc(const char *j, char ***argv_out, int *argc_out);
int hold_read_owned_file_no_symlink(const char *path, char **out);
int hold_fsync_dir_path(const char *dir);
int hold_copy_argv(char ***out, int argc, char **argv);
int hold_read_small_file(const char *path, char **out);
int hold_read_exec_handshake(int fd, int *child_errno);
int hold_base64url_encode(const unsigned char *in, size_t in_len, char *out, size_t out_n);
int hold_base64url_decode(const char *in, unsigned char *out, size_t out_n, size_t *out_len);
int hold_sha256_file_hex(const char *path, char out[PROFILE_HASH_STR_LEN]);
void hold_format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);
void hold_format_relative_age(int64_t start_unix_ns, char *out, size_t n);
bool hold_valid_runid_selector(const char *sel);
int hold_log_idx_path(const char *log_path, char *out, size_t n);
int hold_open_log_index_fd(const char *log_path, int raw_log_fd);
int hold_write_indexed_log_bytes_fd(int log_fd, int idx_fd, const char *stream, const char *data, size_t n);
int hold_write_json_log_entry_fd(int fd, const char *stream, const char *data, size_t n);
int hold_write_json_log_bytes_fd(int fd, const char *stream, const char *data, size_t n);
int hold_decode_json_log_line(const char *line, char **out);
const char *hold_run_id_display(const char *id, char out[ID_DISPLAY_HEX_LEN + 1]);
bool hold_valid_target_atom(const char *id);

#endif /* HOLD_CORE_H */
