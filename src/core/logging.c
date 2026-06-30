#include "hold/config.h"
#include "hold/core.h"

#define HOLD_LOGIDX_MAGIC "HLOGIDX"
#define HOLD_LOGIDX_MAGIC_SIZE 8
#define HOLD_LOGIDX_VERSION 1u
#define HOLD_LOGIDX_HEADER_SIZE 80u
#define HOLD_LOGIDX_ENTRY_SIZE 16u
#define HOLD_LOGIDX_F_LITTLE_ENDIAN 0x00000001u
#define HOLD_LOGIDX_OFFSET_BITS 44
#define HOLD_LOGIDX_LEN_BITS 20
#define HOLD_LOGIDX_TIME_BITS 48
#define HOLD_LOGIDX_META_STREAM_STDERR 0x0001u
#define HOLD_LOGIDX_META_NO_NEWLINE 0x0002u
#define HOLD_LOGIDX_META_CONTINUATION 0x0004u
#define HOLD_LOGIDX_META_TRUNCATED 0x0008u
#define HOLD_LOGIDX_OFFSET_MASK ((1ULL << HOLD_LOGIDX_OFFSET_BITS) - 1ULL)
#define HOLD_LOGIDX_LEN_MASK ((1ULL << HOLD_LOGIDX_LEN_BITS) - 1ULL)
#define HOLD_LOGIDX_TIME_MASK ((1ULL << HOLD_LOGIDX_TIME_BITS) - 1ULL)

struct hold_logidx_header_mem {
    uint64_t base_unix_us;
    uint64_t entry_count;
    uint64_t raw_size_bytes;
    uint64_t raw_mtime_ns;
};

static uint64_t now_unix_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

static uint64_t file_mtime_ns_from_stat(const struct stat *st) {
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ULL + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(st_mtim)
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL + (uint64_t)st->st_mtim.tv_nsec;
#else
    return (uint64_t)st->st_mtime * 1000000000ULL;
#endif
}

static int write_u16_le_at(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    return 0;
}

static int write_u32_le_at(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
    return 0;
}

static int write_u64_le_at(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
    return 0;
}

static uint16_t read_u16_le_at(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t read_u64_le_at(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int write_full_at(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t wr = pwrite(fd, p, n, off);
        if (wr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (wr == 0) { errno = EIO; return -1; }
        p += wr;
        n -= (size_t)wr;
        off += wr;
    }
    return 0;
}

int hold_log_idx_path(const char *log_path, char *out, size_t n) {
    if (!log_path || !*log_path || !out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    return hold_checked_snprintf(out, n, "%s.idx", log_path);
}

static int write_logidx_header(int fd, const struct hold_logidx_header_mem *h) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, HOLD_LOGIDX_MAGIC, 7);
    buf[7] = '\0';
    write_u16_le_at(buf + 8, HOLD_LOGIDX_VERSION);
    write_u16_le_at(buf + 10, HOLD_LOGIDX_HEADER_SIZE);
    write_u16_le_at(buf + 12, HOLD_LOGIDX_ENTRY_SIZE);
    write_u16_le_at(buf + 14, 0);
    write_u32_le_at(buf + 16, HOLD_LOGIDX_F_LITTLE_ENDIAN);
    write_u32_le_at(buf + 20, 0);
    write_u64_le_at(buf + 24, h->base_unix_us);
    write_u64_le_at(buf + 32, h->entry_count);
    write_u64_le_at(buf + 40, h->raw_size_bytes);
    write_u64_le_at(buf + 48, h->raw_mtime_ns);
    return write_full_at(fd, buf, sizeof(buf), 0);
}

static int read_logidx_header(int fd, struct hold_logidx_header_mem *h) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    ssize_t nr;
    do {
        nr = pread(fd, buf, sizeof(buf), 0);
    } while (nr < 0 && errno == EINTR);
    if (nr != (ssize_t)sizeof(buf)) { errno = EINVAL; return -1; }
    if (memcmp(buf, HOLD_LOGIDX_MAGIC, 7) != 0 || read_u16_le_at(buf + 8) != HOLD_LOGIDX_VERSION ||
        read_u16_le_at(buf + 10) != HOLD_LOGIDX_HEADER_SIZE || read_u16_le_at(buf + 12) != HOLD_LOGIDX_ENTRY_SIZE) {
        errno = EINVAL;
        return -1;
    }
    h->base_unix_us = read_u64_le_at(buf + 24);
    h->entry_count = read_u64_le_at(buf + 32);
    h->raw_size_bytes = read_u64_le_at(buf + 40);
    h->raw_mtime_ns = read_u64_le_at(buf + 48);
    return 0;
}

int hold_open_log_index_fd(const char *log_path, int raw_log_fd) {
    char idx_path[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    int fd = open(idx_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (fstat(raw_log_fd, &st) != 0) {
        close(fd);
        return -1;
    }
    struct stat idx_st;
    if (fstat(fd, &idx_st) != 0) {
        close(fd);
        return -1;
    }
    if (idx_st.st_size >= (off_t)HOLD_LOGIDX_HEADER_SIZE) {
        struct hold_logidx_header_mem h;
        if (read_logidx_header(fd, &h) == 0) {
            return fd;
        }
        if (ftruncate(fd, 0) != 0) {
            close(fd);
            return -1;
        }
    }
    struct hold_logidx_header_mem h;
    memset(&h, 0, sizeof(h));
    h.base_unix_us = now_unix_us();
    h.raw_size_bytes = (uint64_t)st.st_size;
    h.raw_mtime_ns = file_mtime_ns_from_stat(&st);
    if (write_logidx_header(fd, &h) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int append_logidx_entry(int idx_fd, uint64_t raw_offset, uint32_t len, uint64_t ts_us, uint32_t meta, uint64_t raw_size_bytes, uint64_t raw_mtime_ns) {
    if (len == 0) return 0;
    struct hold_logidx_header_mem h;
    if (read_logidx_header(idx_fd, &h) != 0) return -1;
    if (h.base_unix_us == 0) h.base_unix_us = ts_us;
    uint64_t delta = ts_us >= h.base_unix_us ? ts_us - h.base_unix_us : 0;
    if (raw_offset > HOLD_LOGIDX_OFFSET_MASK) { errno = EOVERFLOW; return -1; }
    if (len > (1u << HOLD_LOGIDX_LEN_BITS)) {
        len = (1u << HOLD_LOGIDX_LEN_BITS);
        meta |= HOLD_LOGIDX_META_TRUNCATED;
    }
    if (delta > HOLD_LOGIDX_TIME_MASK) { errno = EOVERFLOW; return -1; }
    uint64_t pos_len = (raw_offset & HOLD_LOGIDX_OFFSET_MASK) | (((uint64_t)(len - 1u) & HOLD_LOGIDX_LEN_MASK) << HOLD_LOGIDX_OFFSET_BITS);
    uint64_t time_meta = (delta & HOLD_LOGIDX_TIME_MASK) | (((uint64_t)meta & 0xffffu) << HOLD_LOGIDX_TIME_BITS);
    unsigned char entry[HOLD_LOGIDX_ENTRY_SIZE];
    write_u64_le_at(entry, pos_len);
    write_u64_le_at(entry + 8, time_meta);
    off_t entry_off = (off_t)HOLD_LOGIDX_HEADER_SIZE + (off_t)(h.entry_count * HOLD_LOGIDX_ENTRY_SIZE);
    if (write_full_at(idx_fd, entry, sizeof(entry), entry_off) != 0) return -1;
    h.entry_count++;
    h.raw_size_bytes = raw_size_bytes;
    h.raw_mtime_ns = raw_mtime_ns;
    return write_logidx_header(idx_fd, &h);
}

int hold_write_indexed_log_bytes_fd(int log_fd, int idx_fd, const char *stream, const char *data, size_t n) {
    if (n == 0) return 0;
    if (log_fd < 0 || !stream || !*stream || (!data && n > 0)) { errno = EINVAL; return -1; }
    uint32_t stream_meta = !strcmp(stream, "stderr") ? HOLD_LOGIDX_META_STREAM_STDERR : 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != '\n') continue;
        size_t len = i + 1 - start;
        off_t off = lseek(log_fd, 0, SEEK_END);
        if (off < 0) return -1;
        if (hold_write_all(log_fd, data + start, len) != 0) return -1;
        if (idx_fd >= 0) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            if (fstat(log_fd, &st) == 0) {
                (void)append_logidx_entry(idx_fd, (uint64_t)off, (uint32_t)len, now_unix_us(), stream_meta, (uint64_t)st.st_size, file_mtime_ns_from_stat(&st));
            }
        }
        start = i + 1;
    }
    if (start < n) {
        size_t len = n - start;
        off_t off = lseek(log_fd, 0, SEEK_END);
        if (off < 0) return -1;
        if (hold_write_all(log_fd, data + start, len) != 0) return -1;
        if (idx_fd >= 0) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            if (fstat(log_fd, &st) == 0) {
                (void)append_logidx_entry(idx_fd, (uint64_t)off, (uint32_t)len, now_unix_us(), stream_meta | HOLD_LOGIDX_META_NO_NEWLINE, (uint64_t)st.st_size, file_mtime_ns_from_stat(&st));
            }
        }
    }
    return 0;
}

int hold_write_json_log_entry_fd(int fd, const char *stream, const char *data, size_t n) {
    return hold_write_indexed_log_bytes_fd(fd, -1, stream, data, n);
}

int hold_write_json_log_bytes_fd(int fd, const char *stream, const char *data, size_t n) {
    return hold_write_indexed_log_bytes_fd(fd, -1, stream, data, n);
}

int hold_decode_json_log_line(const char *line, char **out) {
    if (!line || !out) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (line[0] == '{') {
        const char *v = NULL;
        if (hold_json_find_key(line, "log", &v) == 0) {
            size_t cap = strlen(line) + 1;
            char *decoded = malloc(cap);
            if (!decoded) return -1;
            if (hold_json_get_str(line, "log", decoded, cap) == 0) {
                *out = decoded;
                return 1;
            }
            free(decoded);
        }
    }
    char *copy = strdup(line);
    if (!copy) return -1;
    *out = copy;
    return 0;
}

const char *hold_run_id_display(const char *id, char out[ID_DISPLAY_HEX_LEN + 1]) {
    if (!out) return "";
    if (!id) {
        out[0] = '\0';
        return out;
    }
    size_t n = strlen(id);
    if (n > ID_DISPLAY_HEX_LEN) n = ID_DISPLAY_HEX_LEN;
    memcpy(out, id, n);
    out[n] = '\0';
    return out;
}
