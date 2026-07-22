#include "hold/config.h"
#include "hold/core.h"

/*
 * HLOGIDX sidecar: an 80-byte little-endian header followed by packed entries.
 * v1 (16 B): offset:44 | len-1:20, then delta_us:48 | meta:16. v2 (24 B): the
 * same first word, then a full 64-bit nanosecond delta (48-bit ns rolls over
 * at ~78 h), then meta:16 | per-line CRC32:32 with 16 bits spare. The v2
 * header adds a monotonic-clock base alongside the realtime base, a header
 * CRC32 for edit detection, and synthetic/recovered timing-provenance flags.
 * Writers emit v2; a sane pre-existing v1 sidecar keeps appending v1 entries
 * (version/entry_size discipline keeps v1 readable). The writer is
 * append-only — the header is written once, never rewritten — and the entry
 * count derives from st_size, which also floors away any torn tail entry
 * after a crash. Indexing never fails or reorders the raw write: the raw log
 * stays the sole source of truth.
 *
 * Reading self-heals (docs/future/playback.md): a sane sidecar is used
 * as-is; a corrupt one is realigned by anchor-matching its per-line CRCs
 * against a rescan of the log text (rsync-style — unanchored entries drop,
 * unanchored lines get interpolated timing); a missing one (or a corrupt v1,
 * which has no CRCs to anchor) is rebuilt with synthetic timing at 50 ms per
 * line from the log file's birth time. Recovered/rebuilt indexes are
 * rewritten to disk best-effort — always as v2 — and flagged so the chrome
 * can say "timing reconstructed" instead of implying recorded truth.
 */
#define HOLD_LOGIDX_MAGIC "HLOGIDX"
#define HOLD_LOGIDX_V1 1u
#define HOLD_LOGIDX_V2 2u
#define HOLD_LOGIDX_HEADER_SIZE 80u
#define HOLD_LOGIDX_V1_ENTRY_SIZE 16u
#define HOLD_LOGIDX_V2_ENTRY_SIZE 24u
#define HOLD_LOGIDX_F_LITTLE_ENDIAN 0x00000001u
#define HOLD_LOGIDX_F_SYNTHETIC 0x00000002u
#define HOLD_LOGIDX_F_RECOVERED 0x00000004u
#define HOLD_LOGIDX_OFFSET_BITS 44
#define HOLD_LOGIDX_TIME_BITS 48
#define HOLD_LOGIDX_META_STREAM_STDERR 0x0001u
#define HOLD_LOGIDX_META_NO_NEWLINE 0x0002u
#define HOLD_LOGIDX_META_STREAM_PTY 0x0004u
#define HOLD_LOGIDX_META_TRUNCATED 0x0008u
#define HOLD_LOGIDX_META_STREAM_STDIN 0x0010u
#define HOLD_LOGIDX_OFFSET_MASK ((1ULL << HOLD_LOGIDX_OFFSET_BITS) - 1ULL)
#define HOLD_LOGIDX_LEN_MASK ((1ULL << 20) - 1ULL)
#define HOLD_LOGIDX_TIME_MASK ((1ULL << HOLD_LOGIDX_TIME_BITS) - 1ULL)
/* Anchor-match lookahead: how far a garbage CRC can make realignment skip. */
#define HOLD_LOGIDX_ANCHOR_WINDOW 64
#define HOLD_LOGIDX_SYNTH_STEP_NS 50000000ULL

static uint64_t now_unix_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

static uint64_t now_mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void put_le(unsigned char *p, int nbytes, uint64_t v) {
    for (int i = 0; i < nbytes; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static uint64_t get_le(const unsigned char *p, int nbytes) {
    uint64_t v = 0;
    for (int i = nbytes - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* CRC-32 (IEEE 802.3, the zlib polynomial): the "basic tiny hash" the spec
 * anchors realignment on. Streaming form: seed with hold_crc32_begin, feed
 * bytes, finish with hold_crc32_end. */
static uint32_t crc32_table[256];

static void crc32_init(void) {
    if (crc32_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1u) ? 0xedb88320u : 0u);
        crc32_table[i] = c;
    }
}

#define hold_crc32_begin() 0xffffffffu
#define hold_crc32_end(c) ((c) ^ 0xffffffffu)

static uint32_t crc32_feed(uint32_t crc, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    crc32_init();
    for (size_t i = 0; i < n; i++) crc = crc32_table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    return crc;
}

static uint32_t crc32_buf(const void *buf, size_t n) {
    return hold_crc32_end(crc32_feed(hold_crc32_begin(), buf, n));
}

static int write_full_at(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t wr = pwrite(fd, p, n, off);
        if (wr < 0 && errno == EINTR) continue;
        if (wr == 0) errno = EIO;
        if (wr <= 0) return -1;
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

struct logidx_header {
    uint16_t version;
    uint16_t entry_size;
    uint32_t flags;
    uint64_t base_unix_us;
    uint64_t base_mono_ns;
};

/* Validates magic/version/geometry (and, for v2, the header CRC — the edit
 * detector) and returns the parsed header. */
static int read_logidx_header(int fd, struct logidx_header *h) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    ssize_t nr;
    do {
        nr = pread(fd, buf, sizeof(buf), 0);
    } while (nr < 0 && errno == EINTR);
    if (nr != (ssize_t)sizeof(buf) || memcmp(buf, HOLD_LOGIDX_MAGIC, 8) != 0 ||
        get_le(buf + 10, 2) != HOLD_LOGIDX_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }
    h->version = (uint16_t)get_le(buf + 8, 2);
    h->entry_size = (uint16_t)get_le(buf + 12, 2);
    h->flags = (uint32_t)get_le(buf + 16, 4);
    h->base_unix_us = get_le(buf + 24, 8);
    h->base_mono_ns = get_le(buf + 32, 8);
    if (h->version == HOLD_LOGIDX_V1 && h->entry_size == HOLD_LOGIDX_V1_ENTRY_SIZE) return 0;
    if (h->version == HOLD_LOGIDX_V2 && h->entry_size == HOLD_LOGIDX_V2_ENTRY_SIZE) {
        uint32_t want = (uint32_t)get_le(buf + 40, 4);
        memset(buf + 40, 0, 4);
        if (crc32_buf(buf, sizeof(buf)) == want) return 0;
    }
    errno = EINVAL;
    return -1;
}

static int write_logidx_header_v2(int fd, uint64_t base_unix_us, uint64_t base_mono_ns, uint32_t extra_flags) {
    unsigned char buf[HOLD_LOGIDX_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, HOLD_LOGIDX_MAGIC, 8);
    put_le(buf + 8, 2, HOLD_LOGIDX_V2);
    put_le(buf + 10, 2, HOLD_LOGIDX_HEADER_SIZE);
    put_le(buf + 12, 2, HOLD_LOGIDX_V2_ENTRY_SIZE);
    put_le(buf + 16, 4, HOLD_LOGIDX_F_LITTLE_ENDIAN | extra_flags);
    put_le(buf + 24, 8, base_unix_us);
    put_le(buf + 32, 8, base_mono_ns);
    put_le(buf + 40, 4, crc32_buf(buf, sizeof(buf)));
    return write_full_at(fd, buf, sizeof(buf), 0);
}

int hold_open_log_index_fd(const char *log_path, int raw_log_fd) {
    char idx_path[HOLD_PATH_MAX], dir[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    const char *slash = strrchr(idx_path, '/');
    if (!slash || slash == idx_path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    memcpy(dir, idx_path, (size_t)(slash - idx_path));
    dir[slash - idx_path] = '\0';
    /* Open through the directory, refusing symlinks at both components. */
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
    if (fstat(raw_log_fd, &st) != 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    if (st.st_size >= (off_t)HOLD_LOGIDX_HEADER_SIZE) {
        struct logidx_header h;
        if (read_logidx_header(fd, &h) == 0) return fd; /* keep appending in its own version */
        /* Corrupt header: truncate and re-init, never fatal. */
        if (ftruncate(fd, 0) != 0) {
            close(fd);
            return -1;
        }
    }
    if (write_logidx_header_v2(fd, now_unix_us(), now_mono_ns(), 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pack_entry_v2(unsigned char *e, uint64_t raw_offset, uint64_t full_len, uint64_t delta_ns,
                          uint32_t meta, uint32_t crc) {
    uint64_t stored = full_len;
    if (stored > HOLD_LOGIDX_LEN_MASK + 1u) {
        stored = HOLD_LOGIDX_LEN_MASK + 1u; /* oversize records saturate; len is stored as len-1 */
        meta |= HOLD_LOGIDX_META_TRUNCATED;
    }
    put_le(e, 8, (raw_offset & HOLD_LOGIDX_OFFSET_MASK) | (((stored - 1u) & HOLD_LOGIDX_LEN_MASK) << HOLD_LOGIDX_OFFSET_BITS));
    put_le(e + 8, 8, delta_ns);
    put_le(e + 16, 8, ((uint64_t)meta & 0xffffu) | ((uint64_t)crc << 16));
}

static int append_logidx_entry(int idx_fd, uint64_t raw_offset, const char *data, size_t len, uint32_t meta) {
    struct logidx_header h;
    struct stat st;
    if (len == 0) return 0;
    if (read_logidx_header(idx_fd, &h) != 0 || fstat(idx_fd, &st) != 0) return -1;
    if (raw_offset > HOLD_LOGIDX_OFFSET_MASK) {
        errno = EOVERFLOW;
        return -1;
    }
    unsigned char entry[HOLD_LOGIDX_V2_ENTRY_SIZE];
    if (h.version == HOLD_LOGIDX_V1) {
        uint64_t stored = len;
        if (stored > HOLD_LOGIDX_LEN_MASK + 1u) {
            stored = HOLD_LOGIDX_LEN_MASK + 1u;
            meta |= HOLD_LOGIDX_META_TRUNCATED;
        }
        uint64_t ts_us = now_unix_us();
        uint64_t delta = ts_us >= h.base_unix_us ? ts_us - h.base_unix_us : 0;
        if (delta > HOLD_LOGIDX_TIME_MASK) {
            errno = EOVERFLOW;
            return -1;
        }
        put_le(entry, 8, (raw_offset & HOLD_LOGIDX_OFFSET_MASK) | (((stored - 1u) & HOLD_LOGIDX_LEN_MASK) << HOLD_LOGIDX_OFFSET_BITS));
        put_le(entry + 8, 8, (delta & HOLD_LOGIDX_TIME_MASK) | (((uint64_t)meta & 0xffffu) << HOLD_LOGIDX_TIME_BITS));
    } else {
        /* Monotonic-derived delta; realtime-derived fallback when the stored
         * monotonic base is from another boot (log resumed after reboot). */
        uint64_t mono = now_mono_ns();
        uint64_t delta_ns;
        if (h.base_mono_ns && mono >= h.base_mono_ns) {
            delta_ns = mono - h.base_mono_ns;
        } else {
            uint64_t ts_us = now_unix_us();
            delta_ns = ts_us > h.base_unix_us ? (ts_us - h.base_unix_us) * 1000ULL : 0;
        }
        pack_entry_v2(entry, raw_offset, len, delta_ns, meta, crc32_buf(data, len));
    }
    uint64_t entries = st.st_size > (off_t)HOLD_LOGIDX_HEADER_SIZE
        ? ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / h.entry_size : 0;
    return write_full_at(idx_fd, entry, h.entry_size, (off_t)(HOLD_LOGIDX_HEADER_SIZE + entries * h.entry_size));
}

/* Raw write first, then best-effort index append at the pre-write EOF offset:
 * that offset is the viewer's lookup key. Index failures are ignored. */
static int write_indexed_chunk(int log_fd, int idx_fd, const char *data, size_t len, uint32_t meta) {
    off_t off = lseek(log_fd, 0, SEEK_END);
    if (off < 0) return -1;
    if (hold_write_all(log_fd, data, len) != 0) return -1;
    if (idx_fd >= 0) (void)append_logidx_entry(idx_fd, (uint64_t)off, data, len, meta);
    return 0;
}

int hold_write_indexed_log_bytes_fd(int log_fd, int idx_fd, const char *stream, const char *data, size_t n) {
    if (n == 0) return 0;
    if (log_fd < 0 || !stream || !*stream || !data) {
        errno = EINVAL;
        return -1;
    }
    /* The format's four stream tags (stdout is the untagged default). The pty
     * tag marks a terminal recording (docs/future/playback.md: ANSI TUI
     * detected); the stdin tag is annotation, never display (capture
     * invariant: replaying an IN record is corruption). */
    uint32_t stream_meta = 0;
    if (!strcmp(stream, "stderr")) stream_meta = HOLD_LOGIDX_META_STREAM_STDERR;
    else if (!strcmp(stream, "pty")) stream_meta = HOLD_LOGIDX_META_STREAM_PTY;
    else if (!strcmp(stream, "stdin")) stream_meta = HOLD_LOGIDX_META_STREAM_STDIN;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != '\n') continue;
        if (write_indexed_chunk(log_fd, idx_fd, data + start, i + 1 - start, stream_meta) != 0) return -1;
        start = i + 1;
    }
    if (start < n && write_indexed_chunk(log_fd, idx_fd, data + start, n - start, stream_meta | HOLD_LOGIDX_META_NO_NEWLINE) != 0) {
        return -1;
    }
    return 0;
}

void hold_logidx_map_free(struct hold_logidx_map *m) {
    if (!m) return;
    free(m->records);
    memset(m, 0, sizeof(*m));
}

/* ---- self-healing recovery engine --------------------------------------- */

/* One index line during recovery: a rescanned text line (offset/len/crc from
 * the text) that timing gets anchored or interpolated onto. */
struct rescan_line {
    uint64_t offset;
    uint64_t len; /* full byte length including any trailing newline */
    uint64_t delta_ns;
    uint32_t crc;
    uint16_t meta;
    bool anchored;
};

/* Rescans the whole log text into per-line offset/len/CRC triples — the
 * realignment substrate and the synthetic-rebuild skeleton. */
static int rescan_log_lines(int log_fd, struct rescan_line **out, size_t *out_count) {
    struct rescan_line *lines = NULL;
    size_t count = 0, cap = 0;
    unsigned char buf[65536];
    uint64_t pos = 0, line_start = 0, line_len = 0;
    uint32_t crc = hold_crc32_begin();
    bool in_line = false;
    *out = NULL;
    *out_count = 0;
    for (;;) {
        ssize_t nr = pread(log_fd, buf, sizeof(buf), (off_t)pos);
        if (nr < 0 && errno == EINTR) continue;
        if (nr < 0) {
            free(lines);
            return -1;
        }
        if (nr == 0) break;
        for (ssize_t i = 0; i < nr; i++) {
            if (!in_line) {
                in_line = true;
                line_start = pos + (uint64_t)i;
                line_len = 0;
                crc = hold_crc32_begin();
            }
            crc = crc32_feed(crc, buf + i, 1);
            line_len++;
            if (buf[i] != '\n') continue;
            if (count == cap) {
                size_t next = cap ? cap * 2 : 256;
                struct rescan_line *grown = realloc(lines, next * sizeof(*lines));
                if (!grown) {
                    free(lines);
                    errno = ENOMEM;
                    return -1;
                }
                lines = grown;
                cap = next;
            }
            memset(&lines[count], 0, sizeof(lines[count]));
            lines[count].offset = line_start;
            lines[count].len = line_len;
            lines[count].crc = hold_crc32_end(crc);
            lines[count].meta = 0;
            count++;
            in_line = false;
        }
        pos += (uint64_t)nr;
    }
    if (in_line) {
        if (count == cap) {
            struct rescan_line *grown = realloc(lines, (cap ? cap * 2 : 1) * sizeof(*lines));
            if (!grown) {
                free(lines);
                errno = ENOMEM;
                return -1;
            }
            lines = grown;
        }
        memset(&lines[count], 0, sizeof(lines[count]));
        lines[count].offset = line_start;
        lines[count].len = line_len;
        lines[count].crc = hold_crc32_end(crc);
        lines[count].meta = HOLD_LOGIDX_META_NO_NEWLINE;
        count++;
    }
    *out = lines;
    *out_count = count;
    return 0;
}

/* Log-file birth time in Unix µs: statx btime on Linux, st_birthtimespec on
 * macOS/BSD, else mtime minus the synthetic span (50 ms per line). */
static uint64_t log_birth_us(int log_fd, uint64_t fallback_deduct_us) {
#if defined(__linux__) && defined(STATX_BTIME)
    struct statx sx;
    if (statx(log_fd, "", AT_EMPTY_PATH, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME)) {
        return (uint64_t)sx.stx_btime.tv_sec * 1000000ULL + (uint64_t)sx.stx_btime.tv_nsec / 1000ULL;
    }
#elif defined(__APPLE__)
    struct stat bst;
    if (fstat(log_fd, &bst) == 0) {
        return (uint64_t)bst.st_birthtimespec.tv_sec * 1000000ULL + (uint64_t)bst.st_birthtimespec.tv_nsec / 1000ULL;
    }
#endif
    struct stat st;
    if (fstat(log_fd, &st) != 0) return 0;
    uint64_t mtime_us = (uint64_t)st.st_mtime * 1000000ULL;
    return mtime_us > fallback_deduct_us ? mtime_us - fallback_deduct_us : 0;
}

/* Best-effort persist of a recovered/rebuilt index: temp file beside the
 * sidecar, then rename over. Failure is ignored — the in-memory map already
 * healed the read; an unwritable directory only costs the next reader a
 * rescan. Always writes v2. */
static void persist_recovered_index(const char *idx_path, uint64_t base_unix_us, uint64_t base_mono_ns,
                                    uint32_t extra_flags, const struct rescan_line *lines, size_t count) {
    char tmp[HOLD_PATH_MAX];
    if (hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp", idx_path) != 0) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    if (write_logidx_header_v2(fd, base_unix_us, base_mono_ns, extra_flags) != 0) {
        close(fd);
        unlink(tmp);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        unsigned char entry[HOLD_LOGIDX_V2_ENTRY_SIZE];
        pack_entry_v2(entry, lines[i].offset, lines[i].len, lines[i].delta_ns, lines[i].meta, lines[i].crc);
        if (write_full_at(fd, entry, sizeof(entry), (off_t)(HOLD_LOGIDX_HEADER_SIZE + i * sizeof(entry))) != 0) {
            close(fd);
            unlink(tmp);
            return;
        }
    }
    close(fd);
    if (rename(tmp, idx_path) != 0) unlink(tmp);
}

/* A structurally-readable entry salvaged from a damaged sidecar: the CRC is
 * the anchor key, the delta and stream bits are the payload worth saving. */
struct salvaged_entry {
    uint64_t delta_ns;
    uint32_t crc;
    uint16_t meta;
};

/* Reads every complete entry cell from the sidecar without judging content —
 * garbage cells simply never anchor-match. v1 cells carry no CRC (crc 0), so
 * v1 salvage never anchors and corrupt v1 degrades to synthetic rebuild. */
static int salvage_entries(int idx_fd, uint16_t version, uint16_t entry_size,
                           struct salvaged_entry **out, size_t *out_count) {
    struct stat st;
    *out = NULL;
    *out_count = 0;
    if (fstat(idx_fd, &st) != 0) return -1;
    if (st.st_size <= (off_t)HOLD_LOGIDX_HEADER_SIZE) return 0;
    uint64_t count = ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / entry_size;
    if (count == 0) return 0;
    struct salvaged_entry *ents = calloc((size_t)count, sizeof(*ents));
    if (!ents) {
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    for (uint64_t i = 0; i < count; i++) {
        unsigned char buf[HOLD_LOGIDX_V2_ENTRY_SIZE];
        ssize_t nr;
        do {
            nr = pread(idx_fd, buf, entry_size, (off_t)(HOLD_LOGIDX_HEADER_SIZE + i * entry_size));
        } while (nr < 0 && errno == EINTR);
        if (nr != (ssize_t)entry_size) break;
        if (version == HOLD_LOGIDX_V1) {
            uint64_t time_meta = get_le(buf + 8, 8);
            ents[got].delta_ns = (time_meta & HOLD_LOGIDX_TIME_MASK) * 1000ULL;
            ents[got].meta = (uint16_t)((time_meta >> HOLD_LOGIDX_TIME_BITS) & 0xffffu);
            ents[got].crc = 0;
        } else {
            uint64_t meta_crc = get_le(buf + 16, 8);
            ents[got].delta_ns = get_le(buf + 8, 8);
            ents[got].meta = (uint16_t)(meta_crc & 0xffffu);
            ents[got].crc = (uint32_t)((meta_crc >> 16) & 0xffffffffu);
        }
        got++;
    }
    *out = ents;
    *out_count = got;
    return 0;
}

/* CRC-anchor realignment: greedy in-order match of the rescanned line CRCs
 * against the salvaged entry CRCs within a bounded lookahead window. Anchored
 * lines adopt the entry's recorded delta and stream bits; unanchored lines
 * are interpolated between neighboring anchors (50 ms steps past the edges).
 * Returns the anchor count — zero means nothing re-anchored and the caller
 * falls through to synthetic rebuild. */
static size_t realign_lines(struct rescan_line *lines, size_t line_count,
                            const struct salvaged_entry *ents, size_t ent_count) {
    size_t anchors = 0, j = 0;
    for (size_t i = 0; i < line_count && j < ent_count; i++) {
        size_t limit = j + HOLD_LOGIDX_ANCHOR_WINDOW;
        if (limit > ent_count) limit = ent_count;
        for (size_t k = j; k < limit; k++) {
            if (ents[k].crc != lines[i].crc || ents[k].crc == 0) continue;
            lines[i].delta_ns = ents[k].delta_ns;
            lines[i].meta = (uint16_t)((ents[k].meta & ~(HOLD_LOGIDX_META_NO_NEWLINE | HOLD_LOGIDX_META_TRUNCATED)) | lines[i].meta);
            lines[i].anchored = true;
            anchors++;
            j = k + 1;
            break;
        }
    }
    if (anchors == 0) return 0;
    /* Interpolate timing for the lines that did not re-anchor. */
    size_t prev = line_count; /* sentinel: none yet */
    for (size_t i = 0; i <= line_count; i++) {
        if (i < line_count && !lines[i].anchored) continue;
        size_t lo = prev == line_count ? 0 : prev + 1;
        if (i > lo) {
            if (prev == line_count && i < line_count) {
                /* leading run: step back 50 ms per line from the first anchor */
                for (size_t g = lo; g < i; g++) {
                    uint64_t back = (uint64_t)(i - g) * HOLD_LOGIDX_SYNTH_STEP_NS;
                    lines[g].delta_ns = lines[i].delta_ns > back ? lines[i].delta_ns - back : 0;
                }
            } else if (prev != line_count && i == line_count) {
                /* trailing run: step forward 50 ms per line from the last anchor */
                for (size_t g = lo; g < i; g++) {
                    lines[g].delta_ns = lines[prev].delta_ns + (uint64_t)(g - prev) * HOLD_LOGIDX_SYNTH_STEP_NS;
                }
            } else if (prev != line_count) {
                /* interior run: linear interpolation between the two anchors */
                uint64_t da = lines[prev].delta_ns, db = lines[i].delta_ns;
                uint64_t span = db > da ? db - da : 0;
                for (size_t g = lo; g < i; g++) {
                    lines[g].delta_ns = da + span * (uint64_t)(g - prev) / (uint64_t)(i - prev);
                }
            }
        }
        if (i < line_count) prev = i;
    }
    return anchors;
}

/* Builds the returned map from recovered/rebuilt lines. */
static int map_from_lines(struct hold_logidx_map *out, const struct rescan_line *lines, size_t count,
                          uint64_t base_unix_us) {
    struct hold_logidx_record *recs = NULL;
    if (count > 0 && !(recs = calloc(count, sizeof(*recs)))) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        uint64_t len = lines[i].len;
        uint16_t meta = lines[i].meta;
        if (len > HOLD_LOGIDX_LEN_MASK + 1u) {
            len = HOLD_LOGIDX_LEN_MASK + 1u;
            meta |= HOLD_LOGIDX_META_TRUNCATED;
        }
        recs[i].offset = (off_t)lines[i].offset;
        recs[i].len = (uint32_t)len;
        recs[i].ts_us = base_unix_us + lines[i].delta_ns / 1000ULL;
        recs[i].meta = meta;
    }
    out->records = recs;
    out->count = count;
    out->base_unix_us = base_unix_us;
    return 0;
}

/* Synthetic rebuild: one entry per line, 50 ms monotonic spacing from the log
 * file's birth time, marked synthetic so playback says "timing reconstructed". */
static int rebuild_synthetic(const char *idx_path, int log_fd, struct hold_logidx_map *out) {
    struct rescan_line *lines = NULL;
    size_t count = 0;
    if (rescan_log_lines(log_fd, &lines, &count) != 0) return -1;
    uint64_t base = log_birth_us(log_fd, (uint64_t)count * (HOLD_LOGIDX_SYNTH_STEP_NS / 1000ULL));
    for (size_t i = 0; i < count; i++) lines[i].delta_ns = (uint64_t)i * HOLD_LOGIDX_SYNTH_STEP_NS;
    if (map_from_lines(out, lines, count, base) != 0) {
        free(lines);
        return -1;
    }
    out->synthetic = true;
    persist_recovered_index(idx_path, base, 0, HOLD_LOGIDX_F_SYNTHETIC, lines, count);
    free(lines);
    return 0;
}

/* Corrupt-sidecar path: CRC-anchor realignment against the rescanned text,
 * falling through to synthetic rebuild when nothing re-anchors. */
static int recover_corrupt(const char *idx_path, int idx_fd, int log_fd, bool hdr_ok,
                           const struct logidx_header *h, struct hold_logidx_map *out) {
    uint16_t version = hdr_ok ? h->version : HOLD_LOGIDX_V2;
    uint16_t entry_size = hdr_ok ? h->entry_size : HOLD_LOGIDX_V2_ENTRY_SIZE;
    struct salvaged_entry *ents = NULL;
    struct rescan_line *lines = NULL;
    size_t ent_count = 0, line_count = 0;
    if (salvage_entries(idx_fd, version, entry_size, &ents, &ent_count) != 0) return -1;
    if (rescan_log_lines(log_fd, &lines, &line_count) != 0) {
        free(ents);
        return -1;
    }
    size_t anchors = realign_lines(lines, line_count, ents, ent_count);
    free(ents);
    if (anchors == 0) {
        free(lines);
        return rebuild_synthetic(idx_path, log_fd, out);
    }
    uint64_t base_unix_us = hdr_ok ? h->base_unix_us : log_birth_us(log_fd, 0);
    uint64_t base_mono_ns = hdr_ok ? h->base_mono_ns : 0;
    uint32_t flags = HOLD_LOGIDX_F_RECOVERED | (hdr_ok ? (h->flags & HOLD_LOGIDX_F_SYNTHETIC) : 0);
    if (map_from_lines(out, lines, line_count, base_unix_us) != 0) {
        free(lines);
        return -1;
    }
    out->recovered = true;
    out->synthetic = (flags & HOLD_LOGIDX_F_SYNTHETIC) != 0;
    persist_recovered_index(idx_path, base_unix_us, base_mono_ns, flags, lines, line_count);
    free(lines);
    return 0;
}

/* Loads every complete entry as recorded; the entry count floors so a torn
 * tail entry never loads. Also reports the structural facts the sanity check
 * needs: whether offsets stay monotone and where the index's coverage ends. */
static int load_recorded_entries(int idx_fd, const struct logidx_header *h, struct hold_logidx_map *out,
                                 bool *monotone, uint64_t *max_extent, uint32_t *first_crc, uint32_t *last_crc,
                                 uint16_t *first_meta, uint16_t *last_meta) {
    struct stat st;
    *monotone = true;
    *max_extent = 0;
    if (fstat(idx_fd, &st) != 0) return -1;
    uint64_t count = st.st_size > (off_t)HOLD_LOGIDX_HEADER_SIZE
        ? ((uint64_t)st.st_size - HOLD_LOGIDX_HEADER_SIZE) / h->entry_size : 0;
    struct hold_logidx_record *recs = NULL;
    if (count > 0 && !(recs = calloc((size_t)count, sizeof(*recs)))) {
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    uint64_t prev_end = 0;
    for (uint64_t i = 0; i < count; i++) {
        unsigned char buf[HOLD_LOGIDX_V2_ENTRY_SIZE];
        ssize_t nr;
        do {
            nr = pread(idx_fd, buf, h->entry_size, (off_t)(HOLD_LOGIDX_HEADER_SIZE + i * h->entry_size));
        } while (nr < 0 && errno == EINTR);
        if (nr != (ssize_t)h->entry_size) break;
        uint64_t pos_len = get_le(buf, 8);
        uint64_t offset = pos_len & HOLD_LOGIDX_OFFSET_MASK;
        uint32_t len = (uint32_t)(((pos_len >> HOLD_LOGIDX_OFFSET_BITS) & HOLD_LOGIDX_LEN_MASK) + 1U);
        uint64_t delta_ns;
        uint16_t meta;
        uint32_t crc = 0;
        if (h->version == HOLD_LOGIDX_V1) {
            uint64_t time_meta = get_le(buf + 8, 8);
            delta_ns = (time_meta & HOLD_LOGIDX_TIME_MASK) * 1000ULL;
            meta = (uint16_t)((time_meta >> HOLD_LOGIDX_TIME_BITS) & 0xffffu);
        } else {
            uint64_t meta_crc = get_le(buf + 16, 8);
            delta_ns = get_le(buf + 8, 8);
            meta = (uint16_t)(meta_crc & 0xffffu);
            crc = (uint32_t)((meta_crc >> 16) & 0xffffffffu);
        }
        if (offset < prev_end) *monotone = false;
        prev_end = offset + len;
        if (prev_end > *max_extent) *max_extent = prev_end;
        recs[got].offset = (off_t)offset;
        recs[got].len = len;
        recs[got].ts_us = h->base_unix_us + delta_ns / 1000ULL;
        recs[got].meta = meta;
        if (got == 0) {
            *first_crc = crc;
            *first_meta = meta;
        }
        *last_crc = crc;
        *last_meta = meta;
        got++;
    }
    out->records = recs;
    out->count = got;
    out->base_unix_us = h->base_unix_us;
    return 0;
}

/* Verifies a stored per-line CRC against the raw log bytes it claims to cover. */
static bool entry_crc_matches(int log_fd, const struct hold_logidx_record *rec, uint32_t want) {
    unsigned char buf[65536];
    uint32_t crc = hold_crc32_begin();
    uint64_t left = rec->len;
    off_t off = rec->offset;
    while (left > 0) {
        size_t chunk = left < sizeof(buf) ? (size_t)left : sizeof(buf);
        ssize_t nr = pread(log_fd, buf, chunk, off);
        if (nr < 0 && errno == EINTR) continue;
        if (nr <= 0) return false;
        crc = crc32_feed(crc, buf, (size_t)nr);
        left -= (uint64_t)nr;
        off += nr;
    }
    return hold_crc32_end(crc) == want;
}

/*
 * The recovery ladder (docs/future/playback.md): sane -> use; corrupt ->
 * CRC-anchor realignment; missing -> synthetic rebuild. Never fails into
 * user ceremony while the log text itself is readable.
 */
int hold_logidx_map_load(const char *log_path, struct hold_logidx_map *out) {
    if (!log_path || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char idx_path[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) != 0) return -1;
    int log_fd = open(log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int fd = open(idx_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        /* Missing sidecar: rebuild from scratch with synthetic timing. */
        int rc = errno == ENOENT && log_fd >= 0 ? rebuild_synthetic(idx_path, log_fd, out) : -1;
        if (log_fd >= 0) close(log_fd);
        if (rc != 0 && errno == 0) errno = EINVAL;
        return rc;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        if (log_fd >= 0) close(log_fd);
        errno = EINVAL;
        return -1;
    }
    struct logidx_header h;
    memset(&h, 0, sizeof(h));
    bool hdr_ok = st.st_size >= (off_t)HOLD_LOGIDX_HEADER_SIZE && read_logidx_header(fd, &h) == 0;
    bool sane = hdr_ok;
    bool monotone = true;
    uint64_t max_extent = 0;
    uint32_t first_crc = 0, last_crc = 0;
    uint16_t first_meta = 0, last_meta = 0;
    if (hdr_ok && load_recorded_entries(fd, &h, out, &monotone, &max_extent,
                                        &first_crc, &last_crc, &first_meta, &last_meta) != 0) {
        close(fd);
        if (log_fd >= 0) close(log_fd);
        return -1;
    }
    if (sane && !monotone) sane = false;
    /* Bounds and CRC spot checks need the log text; without it (unreadable
     * log) the structural load above is the best truth available. */
    struct stat log_st;
    if (sane && log_fd >= 0 && fstat(log_fd, &log_st) == 0) {
        /* Raw size is read after the entries, so a live writer's newest
         * entries can never look out of bounds. An index behind the text is
         * normal (live tail); an index past the text is corruption. */
        if (max_extent > (uint64_t)log_st.st_size) sane = false;
        if (sane && h.version == HOLD_LOGIDX_V2 && out->count > 0) {
            /* Spot-check the first and last lines: cheap detection of edited
             * or rewritten text whose structure still looks plausible. */
            if (!(first_meta & HOLD_LOGIDX_META_TRUNCATED) && !entry_crc_matches(log_fd, &out->records[0], first_crc))
                sane = false;
            if (sane && !(last_meta & HOLD_LOGIDX_META_TRUNCATED) &&
                !entry_crc_matches(log_fd, &out->records[out->count - 1], last_crc))
                sane = false;
        }
    }
    if (sane) {
        close(fd);
        if (log_fd >= 0) close(log_fd);
        out->synthetic = (h.flags & HOLD_LOGIDX_F_SYNTHETIC) != 0;
        out->recovered = (h.flags & HOLD_LOGIDX_F_RECOVERED) != 0;
        return 0;
    }
    hold_logidx_map_free(out);
    if (log_fd < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    int rc = recover_corrupt(idx_path, fd, log_fd, hdr_ok, &h, out);
    close(fd);
    close(log_fd);
    return rc;
}

const struct hold_logidx_record *hold_logidx_map_find(const struct hold_logidx_map *m, off_t offset) {
    if (!m) return NULL;
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

enum hold_log_stream hold_logidx_record_stream(uint16_t meta) {
    if (meta & HOLD_LOGIDX_META_STREAM_STDIN) return HOLD_LOG_STREAM_STDIN;
    if (meta & HOLD_LOGIDX_META_STREAM_PTY) return HOLD_LOG_STREAM_PTY;
    if (meta & HOLD_LOGIDX_META_STREAM_STDERR) return HOLD_LOG_STREAM_STDERR;
    return HOLD_LOG_STREAM_STDOUT;
}

size_t hold_logidx_format_time(uint64_t ts_us, enum hold_ts_mode mode, bool utc, char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (mode == HOLD_TS_NONE) return 0;
    time_t secs = (time_t)(ts_us / 1000000ULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (utc ? !gmtime_r(&secs, &tm) : !localtime_r(&secs, &tm)) return 0;
    const char *fmt;
    if (mode == HOLD_TS_DATE) {
        fmt = utc ? "%Y-%m-%d %H:%M:%SZ " : "%Y-%m-%d %H:%M:%S ";
    } else {
        fmt = utc ? "%H:%M:%SZ " : "%H:%M:%S ";
    }
    return strftime(out, n, fmt, &tm);
}
