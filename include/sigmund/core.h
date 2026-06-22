#pragma once
#ifndef SIGMUND_CORE_H
#define SIGMUND_CORE_H

#include "sigmund/config.h"
#include "sigmund/types.h"

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t off;
};

void die_errno(const char *msg);
void sig_note(const struct invocation *inv, const char *fmt, ...);
int checked_snprintf(char *dst, size_t n, const char *fmt, ...);
bool has_suffix(const char *s, const char *suffix);
bool valid_id(const char *id);
bool record_json_filename_id(const char *name, char *id, size_t n);
bool valid_id_prefix(const char *id);
bool valid_profile_hash(const char *hash);
bool valid_alias(const char *alias);
bool valid_record(const struct record *r);
int mkdir_p0700(const char *dir);
int read_file_trim(const char *path, char *buf, size_t n);
bool path_exists(const char *path);
void sha256_init(struct sha256_ctx *c);
void sha256_final(struct sha256_ctx *c, unsigned char out[32]);
void hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n);
int rand_bytes(uint8_t *buf, size_t n);
int mkdir_p_mode(const char *dir, mode_t mode);
int parse_uid_env(const char *s, uid_t *out);
int parse_gid_env(const char *s, gid_t *out);
int write_all(int fd, const void *buf, size_t n);
void json_escape(FILE *f, const char *s);
int write_json_argv(FILE *f, int argc, char **argv);
void sha256_update_nul_field(struct sha256_ctx *ctx, const char *s);
int append_cmd_human(char *dst, size_t n, size_t *off, const char *arg);
int format_argv_human(char *dst, size_t n, int argc, char **argv);
const char *skip_ws(const char *p);
int parse_json_string(const char *p, char *out, size_t n, const char **endp);
int skip_json_value(const char **pp);
int json_find_key(const char *j, const char *k, const char **v);
int json_get_i64(const char *j, const char *k, int64_t *out);
int json_get_bool(const char *j, const char *k, bool *out);
int json_get_u64(const char *j, const char *k, uint64_t *out);
int json_get_str(const char *j, const char *k, char *out, size_t n);
int json_get_argv_display(const char *j, char *out, size_t n);
void free_argv_alloc(char **argv, int argc);
int json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out);
int json_get_args_alloc(const char *j, char ***argv_out, int *argc_out);
int read_owned_file_no_symlink(const char *path, char **out);
int fsync_dir_path(const char *dir);
int copy_argv(char ***out, int argc, char **argv);
int read_small_file(const char *path, char **out);
int read_exec_handshake(int fd, int *child_errno);
void format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);
void format_relative_age(int64_t start_unix_ns, char *out, size_t n);
bool valid_runid_selector(const char *sel);
bool valid_target_atom(const char *id);

#endif /* SIGMUND_CORE_H */
