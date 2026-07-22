#include "hold/config.h"
#include "hold/core.h"

/*
 * Pins the HLOGIDX v2 sidecar format and every rung of the self-healing
 * recovery ladder (docs/future/playback.md): sane -> use, corrupt ->
 * CRC-anchor realignment, missing -> synthetic rebuild. Byte offsets and the
 * CRC polynomial are recomputed here independently so the on-disk format
 * cannot drift silently.
 */

static int failures = 0;

#define EXPECT_TRUE(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, name); \
        failures++; \
    } \
} while (0)

/* Independent CRC-32 (IEEE 802.3): pins the polynomial the sidecar uses. */
static uint32_t tcrc32(const void *buf, size_t n) {
    uint32_t crc = 0xffffffffu;
    const unsigned char *p = (const unsigned char *)buf;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

static uint64_t tget_le(const unsigned char *p, int nbytes) {
    uint64_t v = 0;
    for (int i = nbytes - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void tput_le(unsigned char *p, int nbytes, uint64_t v) {
    for (int i = 0; i < nbytes; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static void write_all_or_die(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(2);
        }
        p += w;
        n -= (size_t)w;
    }
}

static size_t read_file(const char *path, unsigned char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, cap);
    close(fd);
    return n < 0 ? 0 : (size_t)n;
}

static void make_temp_dir(char *dir) {
    strcpy(dir, "/tmp/hold-logidx-test.XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(2);
    }
}

static void remove_dir(const char *dir) {
    char cmd[HOLD_PATH_MAX + 16];
    if (hold_checked_snprintf(cmd, sizeof(cmd), "rm -rf %s", dir) == 0) {
        if (system(cmd) != 0) { /* best effort cleanup */ }
    }
}

/* Builds a raw log plus sidecar through the real writer, one indexed record
 * per line; returns the open raw-log fd. */
static int build_indexed_log(const char *dir, char *log_path_out, size_t cap,
                             const char *const *lines, const char *const *streams, size_t n) {
    if (hold_checked_snprintf(log_path_out, cap, "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path_out, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (logfd < 0) {
        perror("open log");
        exit(2);
    }
    int idxfd = hold_open_log_index_fd(log_path_out, logfd);
    if (idxfd < 0) {
        perror("open idx");
        exit(2);
    }
    for (size_t i = 0; i < n; i++) {
        if (hold_write_indexed_log_bytes_fd(logfd, idxfd, streams ? streams[i] : "stdout",
                                            lines[i], strlen(lines[i])) != 0) {
            perror("write indexed");
            exit(2);
        }
    }
    close(idxfd);
    return logfd;
}

/* Hand-crafts a v1 sidecar byte-by-byte: the compatibility fixture. */
static void write_v1_sidecar(const char *idx_path, uint64_t base_us, const uint64_t *offsets,
                             const uint64_t *lens, const uint64_t *delta_us, const uint16_t *metas, size_t n) {
    unsigned char hdr[80];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "HLOGIDX", 8);
    tput_le(hdr + 8, 2, 1);   /* version */
    tput_le(hdr + 10, 2, 80); /* header size */
    tput_le(hdr + 12, 2, 16); /* entry size */
    tput_le(hdr + 16, 4, 1);  /* little-endian flag */
    tput_le(hdr + 24, 8, base_us);
    int fd = open(idx_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("open v1 idx");
        exit(2);
    }
    write_all_or_die(fd, hdr, sizeof(hdr));
    for (size_t i = 0; i < n; i++) {
        unsigned char e[16];
        tput_le(e, 8, (offsets[i] & ((1ULL << 44) - 1)) | ((lens[i] - 1) << 44));
        tput_le(e + 8, 8, (delta_us[i] & ((1ULL << 48) - 1)) | ((uint64_t)metas[i] << 48));
        write_all_or_die(fd, e, sizeof(e));
    }
    close(fd);
}

static void test_v2_header_and_entry_format(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"alpha\n", "beta\n"};
    const char *streams[] = {"stdout", "stderr"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, streams, 2);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);

    unsigned char buf[256];
    size_t n = read_file(idx_path, buf, sizeof(buf));
    EXPECT_TRUE("v2 sidecar is header plus two 24B entries", n == 80 + 2 * 24);
    EXPECT_TRUE("magic", memcmp(buf, "HLOGIDX", 8) == 0);
    EXPECT_TRUE("version is 2", tget_le(buf + 8, 2) == 2);
    EXPECT_TRUE("header size is 80", tget_le(buf + 10, 2) == 80);
    EXPECT_TRUE("entry size is 24", tget_le(buf + 12, 2) == 24);
    EXPECT_TRUE("little-endian flag set, provenance flags clear", tget_le(buf + 16, 4) == 1);
    uint64_t base_us = tget_le(buf + 24, 8);
    uint64_t base_mono = tget_le(buf + 32, 8);
    EXPECT_TRUE("realtime base set", base_us > 0);
    EXPECT_TRUE("monotonic base set", base_mono > 0);
    uint32_t want = (uint32_t)tget_le(buf + 40, 4);
    unsigned char hdr[80];
    memcpy(hdr, buf, 80);
    memset(hdr + 40, 0, 4);
    EXPECT_TRUE("header CRC32 verifies", tcrc32(hdr, 80) == want);

    /* entry 0: "alpha\n" at offset 0, len 6, stdout */
    uint64_t w0 = tget_le(buf + 80, 8);
    EXPECT_TRUE("entry0 offset 0", (w0 & ((1ULL << 44) - 1)) == 0);
    EXPECT_TRUE("entry0 len-1 is 5", (w0 >> 44) == 5);
    EXPECT_TRUE("entry0 ns delta under a second", tget_le(buf + 88, 8) < 1000000000ULL);
    uint64_t w2 = tget_le(buf + 96, 8);
    EXPECT_TRUE("entry0 meta stdout", (w2 & 0xffffu) == 0);
    EXPECT_TRUE("entry0 per-line CRC32", ((w2 >> 16) & 0xffffffffu) == tcrc32("alpha\n", 6));
    EXPECT_TRUE("entry0 spare bits zero", (w2 >> 48) == 0);
    /* entry 1: "beta\n" at offset 6, len 5, stderr */
    uint64_t e1w0 = tget_le(buf + 104, 8);
    uint64_t e1w2 = tget_le(buf + 120, 8);
    EXPECT_TRUE("entry1 offset 6", (e1w0 & ((1ULL << 44) - 1)) == 6);
    EXPECT_TRUE("entry1 len-1 is 4", (e1w0 >> 44) == 4);
    EXPECT_TRUE("entry1 meta stderr", (e1w2 & 0xffffu) == 1);
    EXPECT_TRUE("entry1 per-line CRC32", ((e1w2 >> 16) & 0xffffffffu) == tcrc32("beta\n", 5));
    EXPECT_TRUE("entry deltas do not decrease", tget_le(buf + 112, 8) >= tget_le(buf + 88, 8));

    close(logfd);
    remove_dir(dir);
}

static void test_v2_roundtrip_map_load(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"out one\n", "err two\n"};
    const char *streams[] = {"stdout", "stderr"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, streams, 2);

    struct hold_logidx_map map;
    EXPECT_TRUE("sane v2 loads", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("two records", map.count == 2);
    EXPECT_TRUE("recorded timing is not flagged", !map.synthetic && !map.recovered);
    EXPECT_TRUE("base set", map.base_unix_us > 0);
    const struct hold_logidx_record *r0 = hold_logidx_map_find(&map, 0);
    const struct hold_logidx_record *r1 = hold_logidx_map_find(&map, 8);
    EXPECT_TRUE("offset 0 found", r0 && r0->len == 8);
    EXPECT_TRUE("offset 8 found", r1 && r1->len == 8);
    if (r0 && r1) {
        EXPECT_TRUE("streams preserved", hold_logidx_record_stream(r0->meta) == HOLD_LOG_STREAM_STDOUT &&
                        hold_logidx_record_stream(r1->meta) == HOLD_LOG_STREAM_STDERR);
        EXPECT_TRUE("timestamps do not decrease", r1->ts_us >= r0->ts_us);
        EXPECT_TRUE("timestamps derive from the base", r0->ts_us >= map.base_unix_us);
    }
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

/* The format's reserved pty stream tag (a terminal recording, playback
 * spec): written as meta bit 0x4, decoded as HOLD_LOG_STREAM_PTY, and
 * preserved through CRC-anchor recovery like the other stream bits. */
static void test_pty_stream_tag_roundtrip_and_recovery(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"plain lead-in\n", "\033[?1049h\033[2;2Hframe\n"};
    const char *streams[] = {"stdout", "pty"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, streams, 2);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);

    unsigned char buf[256];
    size_t n = read_file(idx_path, buf, sizeof(buf));
    EXPECT_TRUE("two entries on disk", n == 80 + 2 * 24);
    EXPECT_TRUE("pty meta bit 0x4 on entry 1", (tget_le(buf + 120, 8) & 0xffffu) == 0x4);

    struct hold_logidx_map map;
    EXPECT_TRUE("pty-tagged sidecar loads", hold_logidx_map_load(log_path, &map) == 0 && map.count == 2);
    EXPECT_TRUE("streams decode stdout then pty", map.count == 2 &&
                    hold_logidx_record_stream(map.records[0].meta) == HOLD_LOG_STREAM_STDOUT &&
                    hold_logidx_record_stream(map.records[1].meta) == HOLD_LOG_STREAM_PTY);
    hold_logidx_map_free(&map);

    /* Tear an offset word: recovery must keep the pty tag on the re-anchored
     * line (the log stays self-describing as a terminal recording). */
    int fd = open(idx_path, O_WRONLY | O_CLOEXEC);
    EXPECT_TRUE("open idx for corruption", fd >= 0);
    unsigned char junk[8];
    memset(junk, 0xff, sizeof(junk));
    EXPECT_TRUE("corrupt entry offset word", pwrite(fd, junk, sizeof(junk), 80) == 8);
    close(fd);
    EXPECT_TRUE("corrupt pty sidecar recovers", hold_logidx_map_load(log_path, &map) == 0 && map.recovered);
    EXPECT_TRUE("pty tag survives recovery", map.count == 2 &&
                    hold_logidx_record_stream(map.records[1].meta) == HOLD_LOG_STREAM_PTY);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_v1_sidecar_still_readable(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    write_all_or_die(logfd, "v-one\nv-two\n", 12);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);
    uint64_t offsets[] = {0, 6}, lens[] = {6, 6}, deltas[] = {1000, 2000};
    uint16_t metas[] = {0, 1};
    write_v1_sidecar(idx_path, 1600000000000000ULL, offsets, lens, deltas, metas, 2);

    struct hold_logidx_map map;
    EXPECT_TRUE("v1 loads", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("v1 has two records", map.count == 2);
    EXPECT_TRUE("v1 timing is recorded, not flagged", !map.synthetic && !map.recovered);
    EXPECT_TRUE("v1 microsecond deltas", map.count == 2 && map.records[0].ts_us == 1600000000001000ULL &&
                    map.records[1].ts_us == 1600000000002000ULL);
    EXPECT_TRUE("v1 stderr meta", map.count == 2 && hold_logidx_record_stream(map.records[1].meta) == HOLD_LOG_STREAM_STDERR);
    unsigned char buf[96];
    EXPECT_TRUE("sane v1 stays v1 on disk", read_file(idx_path, buf, sizeof(buf)) == 96 && tget_le(buf + 8, 2) == 1);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_v1_append_discipline_stays_v1(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);
    write_v1_sidecar(idx_path, 1600000000000000ULL, NULL, NULL, NULL, NULL, 0);

    int idxfd = hold_open_log_index_fd(log_path, logfd);
    EXPECT_TRUE("writer accepts existing v1 sidecar", idxfd >= 0);
    EXPECT_TRUE("append through v1 sidecar", hold_write_indexed_log_bytes_fd(logfd, idxfd, "stdout", "hello\n", 6) == 0);
    if (idxfd >= 0) close(idxfd);
    unsigned char buf[128];
    EXPECT_TRUE("v1 grows by one 16B entry", read_file(idx_path, buf, sizeof(buf)) == 96);
    EXPECT_TRUE("v1 version preserved", tget_le(buf + 8, 2) == 1);
    struct hold_logidx_map map;
    EXPECT_TRUE("appended v1 loads", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("appended v1 record", map.count == 1 && map.records[0].len == 6);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_missing_sidecar_synthetic_rebuild(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    write_all_or_die(logfd, "syn one\nsyn two\ntail", 20);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);

    struct hold_logidx_map map;
    EXPECT_TRUE("missing sidecar rebuilds", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("one entry per line", map.count == 3);
    EXPECT_TRUE("synthetic timing flagged", map.synthetic && !map.recovered);
    EXPECT_TRUE("offsets re-derived", map.count == 3 && map.records[0].offset == 0 &&
                    map.records[1].offset == 8 && map.records[2].offset == 16);
    EXPECT_TRUE("50ms monotonic spacing", map.count == 3 &&
                    map.records[1].ts_us - map.records[0].ts_us == 50000 &&
                    map.records[2].ts_us - map.records[1].ts_us == 50000);
    EXPECT_TRUE("base from file birth time is sane", map.base_unix_us > 0 && map.records[0].ts_us <= (uint64_t)time(NULL) * 1000000ULL + 1000000ULL);
    hold_logidx_map_free(&map);

    unsigned char buf[256];
    size_t n = read_file(idx_path, buf, sizeof(buf));
    EXPECT_TRUE("synthetic sidecar persisted as v2", n == 80 + 3 * 24 && tget_le(buf + 8, 2) == 2);
    EXPECT_TRUE("synthetic flag on disk", (tget_le(buf + 16, 4) & 0x2) != 0);
    EXPECT_TRUE("tail entry marked no-newline", (tget_le(buf + 80 + 2 * 24 + 16, 8) & 0x2) != 0);
    EXPECT_TRUE("persisted sidecar reloads as synthetic",
                hold_logidx_map_load(log_path, &map) == 0 && map.count == 3 && map.synthetic && !map.recovered);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_synthetic_rebuild_in_unwritable_dir_still_loads(void) {
    if (geteuid() == 0) return; /* root ignores directory modes */
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    write_all_or_die(logfd, "ro one\nro two\n", 14);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);
    EXPECT_TRUE("make dir read-only", chmod(dir, 0500) == 0);

    struct hold_logidx_map map;
    EXPECT_TRUE("rebuild succeeds without persisting", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("read-only rebuild is synthetic", map.count == 2 && map.synthetic);
    EXPECT_TRUE("no sidecar written", access(idx_path, F_OK) != 0);
    hold_logidx_map_free(&map);
    EXPECT_TRUE("restore dir mode", chmod(dir, 0700) == 0);
    close(logfd);
    remove_dir(dir);
}

static void test_corrupt_entry_realigns_and_preserves_timing(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"heal one\n", "heal two\n", "heal three\n"};
    const char *streams[] = {"stdout", "stderr", "stdout"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, streams, 3);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);

    struct hold_logidx_map before;
    EXPECT_TRUE("pristine load", hold_logidx_map_load(log_path, &before) == 0 && before.count == 3);
    uint64_t ts0 = before.count == 3 ? before.records[0].ts_us : 0;
    uint64_t ts2 = before.count == 3 ? before.records[2].ts_us : 0;
    hold_logidx_map_free(&before);

    /* Tear entry 0's offset word; its per-line CRC stays intact. */
    int fd = open(idx_path, O_WRONLY | O_CLOEXEC);
    EXPECT_TRUE("open idx for corruption", fd >= 0);
    unsigned char junk[8];
    memset(junk, 0xff, sizeof(junk));
    EXPECT_TRUE("corrupt entry offset word", pwrite(fd, junk, sizeof(junk), 80) == 8);
    close(fd);

    struct hold_logidx_map map;
    EXPECT_TRUE("corrupt sidecar recovers", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("all lines re-anchored", map.count == 3);
    EXPECT_TRUE("recovered flag set", map.recovered && !map.synthetic);
    EXPECT_TRUE("offsets re-derived from the text", map.count == 3 && map.records[0].offset == 0 &&
                    map.records[1].offset == 9 && map.records[2].offset == 18);
    EXPECT_TRUE("recorded deltas preserved through recovery", map.count == 3 &&
                    map.records[0].ts_us == ts0 && map.records[2].ts_us == ts2);
    EXPECT_TRUE("stream bits preserved through recovery", map.count == 3 &&
                    hold_logidx_record_stream(map.records[1].meta) == HOLD_LOG_STREAM_STDERR);
    hold_logidx_map_free(&map);

    unsigned char buf[256];
    size_t n = read_file(idx_path, buf, sizeof(buf));
    EXPECT_TRUE("healed sidecar rewritten as v2", n == 80 + 3 * 24 && tget_le(buf + 8, 2) == 2);
    EXPECT_TRUE("recovered flag on disk", (tget_le(buf + 16, 4) & 0x4) != 0);
    EXPECT_TRUE("healed sidecar reloads sane and labeled",
                hold_logidx_map_load(log_path, &map) == 0 && map.count == 3 && map.recovered);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_edited_log_interpolates_inserted_line(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"alpha\n", "beta\n", "gamma\n"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, NULL, 3);
    /* Edit the text: insert a line the index has never seen. */
    EXPECT_TRUE("rewrite log text", ftruncate(logfd, 0) == 0 && lseek(logfd, 0, SEEK_SET) == 0);
    write_all_or_die(logfd, "alpha\ninserted\nbeta\ngamma\n", 26);

    struct hold_logidx_map map;
    EXPECT_TRUE("edited log recovers", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("all four lines indexed", map.count == 4);
    EXPECT_TRUE("recovered flag set", map.recovered && !map.synthetic);
    EXPECT_TRUE("edited offsets re-derived", map.count == 4 && map.records[0].offset == 0 &&
                    map.records[1].offset == 6 && map.records[2].offset == 15 && map.records[3].offset == 20);
    EXPECT_TRUE("inserted line timing interpolated between anchors", map.count == 4 &&
                    map.records[0].ts_us <= map.records[1].ts_us && map.records[1].ts_us <= map.records[2].ts_us &&
                    map.records[2].ts_us <= map.records[3].ts_us);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_truncated_log_drops_unanchored_entries(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"one\n", "two\n", "three\n", "four\n"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, NULL, 4);
    EXPECT_TRUE("truncate log to two lines", ftruncate(logfd, 8) == 0);

    struct hold_logidx_map map;
    EXPECT_TRUE("truncated log recovers", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("index dropped to surviving text", map.count == 2);
    EXPECT_TRUE("recovered flag set", map.recovered);
    EXPECT_TRUE("surviving offsets", map.count == 2 && map.records[0].offset == 0 && map.records[1].offset == 4);
    hold_logidx_map_free(&map);
    close(logfd);
    remove_dir(dir);
}

static void test_corrupt_v1_degrades_to_synthetic(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/run.log", dir) != 0) exit(2);
    int logfd = open(log_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_TRUE("create raw log", logfd >= 0);
    write_all_or_die(logfd, "d-one\nd-two\n", 12);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);
    /* Offsets far past the text: structurally corrupt, and v1 has no CRCs. */
    uint64_t offsets[] = {4000, 5000}, lens[] = {6, 6}, deltas[] = {1000, 2000};
    uint16_t metas[] = {0, 0};
    write_v1_sidecar(idx_path, 1600000000000000ULL, offsets, lens, deltas, metas, 2);

    struct hold_logidx_map map;
    EXPECT_TRUE("corrupt v1 rebuilds", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("degrades to synthetic (no CRCs to anchor)", map.synthetic && !map.recovered);
    EXPECT_TRUE("synthetic entries per line", map.count == 2 &&
                    map.records[1].ts_us - map.records[0].ts_us == 50000);
    hold_logidx_map_free(&map);
    unsigned char buf[160];
    EXPECT_TRUE("rebuild persisted as v2", read_file(idx_path, buf, sizeof(buf)) == 80 + 2 * 24 && tget_le(buf + 8, 2) == 2);
    close(logfd);
    remove_dir(dir);
}

static void test_corrupt_header_recovers_via_crc_anchors(void) {
    char dir[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX], idx_path[HOLD_PATH_MAX];
    make_temp_dir(dir);
    const char *lines[] = {"hdr one\n", "hdr two\n"};
    int logfd = build_indexed_log(dir, log_path, sizeof(log_path), lines, NULL, 2);
    EXPECT_TRUE("format idx path", hold_log_idx_path(log_path, idx_path, sizeof(idx_path)) == 0);
    int fd = open(idx_path, O_WRONLY | O_CLOEXEC);
    EXPECT_TRUE("open idx for header corruption", fd >= 0);
    EXPECT_TRUE("smash the magic", pwrite(fd, "X", 1, 0) == 1);
    close(fd);

    struct hold_logidx_map map;
    EXPECT_TRUE("headerless sidecar recovers", hold_logidx_map_load(log_path, &map) == 0);
    EXPECT_TRUE("entries re-anchored by CRC alone", map.count == 2 && map.recovered);
    EXPECT_TRUE("offsets re-derived", map.count == 2 && map.records[0].offset == 0 && map.records[1].offset == 8);
    EXPECT_TRUE("recovered timing does not decrease", map.count == 2 && map.records[1].ts_us >= map.records[0].ts_us);
    hold_logidx_map_free(&map);
    unsigned char buf[160];
    EXPECT_TRUE("header rewritten sane", read_file(idx_path, buf, sizeof(buf)) == 80 + 2 * 24 &&
                    memcmp(buf, "HLOGIDX", 8) == 0 && tget_le(buf + 8, 2) == 2);
    close(logfd);
    remove_dir(dir);
}

int main(void) {
    test_v2_header_and_entry_format();
    test_v2_roundtrip_map_load();
    test_pty_stream_tag_roundtrip_and_recovery();
    test_v1_sidecar_still_readable();
    test_v1_append_discipline_stays_v1();
    test_missing_sidecar_synthetic_rebuild();
    test_synthetic_rebuild_in_unwritable_dir_still_loads();
    test_corrupt_entry_realigns_and_preserves_timing();
    test_edited_log_interpolates_inserted_line();
    test_truncated_log_drops_unanchored_entries();
    test_corrupt_v1_degrades_to_synthetic();
    test_corrupt_header_recovers_via_crc_anchors();
    if (failures) {
        fprintf(stderr, "logidx_recovery_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: sidecar v2 format and recovery ladder");
    return 0;
}
