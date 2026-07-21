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

static int open_log_index_no_symlink(const char *idx_path) {
    if (!idx_path || !*idx_path) {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(idx_path, '/');
    if (!slash || slash == idx_path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    size_t dir_len = (size_t)(slash - idx_path);
    if (dir_len >= HOLD_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char dir[HOLD_PATH_MAX];
    memcpy(dir, idx_path, dir_len);
    dir[dir_len] = '\0';

    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    int fd = openat(dirfd, slash + 1, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    int saved = errno;
    close(dirfd);
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int hold_open_log_index_fd(const char *log_path, int raw_log_fd) {
    char idx_path[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    int fd = open_log_index_no_symlink(idx_path);
    if (fd < 0) return -1;
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (fstat(raw_log_fd, &st) != 0) {
        close(fd);
        return -1;
    }
    struct stat idx_st;
    if (fstat(fd, &idx_st) != 0 || !S_ISREG(idx_st.st_mode)) {
        int saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
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

enum hold_log_stream hold_logidx_record_stream(uint16_t meta) {
    return (meta & HOLD_LOGIDX_META_STREAM_STDERR) ? HOLD_LOG_STREAM_STDERR : HOLD_LOG_STREAM_STDOUT;
}

size_t hold_logidx_format_time(uint64_t ts_us, enum hold_ts_mode mode, bool utc, char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (mode == HOLD_TS_NONE) return 0;
    time_t secs = (time_t)(ts_us / 1000000ULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (utc) {
        if (!gmtime_r(&secs, &tm)) return 0;
    } else if (!localtime_r(&secs, &tm)) {
        return 0;
    }
    const char *fmt;
    if (mode == HOLD_TS_DATE) {
        fmt = utc ? "%Y-%m-%d %H:%M:%SZ " : "%Y-%m-%d %H:%M:%S ";
    } else {
        fmt = utc ? "%H:%M:%SZ " : "%H:%M:%S ";
    }
    return strftime(out, n, fmt, &tm);
}

void hold_logidx_map_free(struct hold_logidx_map *m) {
    if (!m) return;
    free(m->records);
    m->records = NULL;
    m->count = 0;
    m->base_unix_us = 0;
}

int hold_logidx_map_load(const char *log_path, struct hold_logidx_map *out) {
    if (!log_path || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char idx_path[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    int fd = open(idx_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    struct hold_logidx_header_mem h;
    if (st.st_size < (off_t)HOLD_LOGIDX_HEADER_SIZE || read_logidx_header(fd, &h) != 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    /* Crash-recovery rule: trust the smaller of the header count and the count
     * physically present in the file. */
    uint64_t physical = 0;
    if ((uint64_t)st.st_size > HOLD_LOGIDX_HEADER_SIZE) {
        physical = ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / HOLD_LOGIDX_ENTRY_SIZE;
    }
    uint64_t count = h.entry_count < physical ? h.entry_count : physical;
    struct hold_logidx_record *recs = NULL;
    if (count > 0) {
        recs = calloc((size_t)count, sizeof(*recs));
        if (!recs) {
            close(fd);
            errno = ENOMEM;
            return -1;
        }
    }
    size_t got = 0;
    for (uint64_t i = 0; i < count; i++) {
        unsigned char buf[HOLD_LOGIDX_ENTRY_SIZE];
        off_t eoff = (off_t)HOLD_LOGIDX_HEADER_SIZE + (off_t)(i * HOLD_LOGIDX_ENTRY_SIZE);
        ssize_t nr;
        do {
            nr = pread(fd, buf, sizeof(buf), eoff);
        } while (nr < 0 && errno == EINTR);
        if (nr != (ssize_t)sizeof(buf)) break;
        uint64_t pos_len = read_u64_le_at(buf);
        uint64_t time_meta = read_u64_le_at(buf + 8);
        recs[got].offset = (off_t)(pos_len & HOLD_LOGIDX_OFFSET_MASK);
        recs[got].len = (uint32_t)(((pos_len >> HOLD_LOGIDX_OFFSET_BITS) & HOLD_LOGIDX_LEN_MASK) + 1U);
        recs[got].ts_us = h.base_unix_us + (time_meta & HOLD_LOGIDX_TIME_MASK);
        recs[got].meta = (uint16_t)((time_meta >> HOLD_LOGIDX_TIME_BITS) & 0xffffu);
        got++;
    }
    close(fd);
    out->records = recs;
    out->count = got;
    out->base_unix_us = h.base_unix_us;
    return 0;
}

const struct hold_logidx_record *hold_logidx_map_find(const struct hold_logidx_map *m, off_t offset) {
    if (!m || m->count == 0) return NULL;
    size_t lo = 0, hi = m->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->records[mid].offset == offset) return &m->records[mid];
        if (m->records[mid].offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
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
