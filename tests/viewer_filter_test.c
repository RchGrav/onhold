#include "sigmund/config.h"
#include "sigmund/log_viewer.h"

static int failures = 0;

#define EXPECT_TRUE(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, name); \
        failures++; \
    } \
} while (0)

static int temp_log_fd(void) {
    char tmpl[] = "/tmp/sigmund-viewer-test.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    unlink(tmpl);
    return fd;
}

static void write_all_or_die(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(2);
        }
        s += w;
        n -= (size_t)w;
    }
}

static void rewind_or_die(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        exit(2);
    }
}

static void test_literal_filter(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "alpha\nneedle one\nbeta\nneedle two\n");
    rewind_or_die(fd);

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    opts.literal = "needle";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct sigmund_log_filter_result result;
    EXPECT_TRUE("literal filter succeeds", sigmund_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("literal returns two lines", result.line_count == 2);
    EXPECT_TRUE("first literal line", result.line_count > 0 && strstr(result.lines[0], "needle one"));
    EXPECT_TRUE("second literal line", result.line_count > 1 && strstr(result.lines[1], "needle two"));
    EXPECT_TRUE("visible offsets are recorded", result.line_count > 1 && result.line_offsets[0] < result.line_offsets[1]);
    EXPECT_TRUE("next offset advances", result.next_offset > result.line_offsets[1]);
    sigmund_log_filter_result_free(&result);
    close(fd);
}

static void test_similarity_filter(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd,
                     "info request completed normally\n"
                     "warn database connection timeout retrying\n"
                     "debug cache warmed successfully\n");
    rewind_or_die(fd);

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    opts.similar_examples[0] = "error database connection timeout";
    opts.similar_example_count = 1;
    opts.similar_threshold = 0.45;
    opts.max_results = 1;
    opts.visible_capacity = 1;
    struct sigmund_log_filter_result result;
    EXPECT_TRUE("similar filter succeeds", sigmund_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("similar returns one line", result.line_count == 1);
    EXPECT_TRUE("similar line selected", result.line_count > 0 && strstr(result.lines[0], "database connection timeout"));
    sigmund_log_filter_result_free(&result);
    close(fd);
}

static void test_match_ring_wraps(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "hit-0\nhit-1\nhit-2\nhit-3\n");
    rewind_or_die(fd);

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    opts.literal = "hit";
    opts.max_results = 4;
    opts.visible_capacity = 4;
    opts.match_ring_capacity = 2;
    struct sigmund_log_filter_result result;
    EXPECT_TRUE("ring filter succeeds", sigmund_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("all matches counted", result.match_count == 4);
    EXPECT_TRUE("ring capped", result.match_ring_count == 2);
    EXPECT_TRUE("ring start advanced", result.match_ring_start == 0 || result.match_ring_start == 1);
    sigmund_log_filter_result_free(&result);
    close(fd);
}

static void test_lazy_large_file_stops_after_first_screen(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "match early 1\nmatch early 2\n");
    for (int i = 0; i < 20000; i++) {
        write_all_or_die(fd, "boring filler line that must not be scanned before first screen\n");
    }
    off_t size = lseek(fd, 0, SEEK_END);
    rewind_or_die(fd);

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    opts.literal = "match early";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct sigmund_log_filter_result result;
    EXPECT_TRUE("lazy large filter succeeds", sigmund_log_filter_fd(fd, &opts, &result) == 0);
    EXPECT_TRUE("first screen has two matches", result.line_count == 2);
    EXPECT_TRUE("did not scan whole file", result.bytes_read < (size_t)size / 4);
    EXPECT_TRUE("did not reach eof", !result.reached_eof);
    sigmund_log_filter_result_free(&result);
    close(fd);
}

static void test_next_offset_resumes_next_screen(void) {
    int fd = temp_log_fd();
    write_all_or_die(fd, "hit one\nhit two\nhit three\nhit four\n");
    rewind_or_die(fd);

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    opts.literal = "hit";
    opts.max_results = 2;
    opts.visible_capacity = 2;
    struct sigmund_log_filter_result first;
    EXPECT_TRUE("first page succeeds", sigmund_log_filter_fd(fd, &opts, &first) == 0);
    EXPECT_TRUE("first page has first hit", first.line_count > 0 && strstr(first.lines[0], "hit one"));
    EXPECT_TRUE("first page has second hit", first.line_count > 1 && strstr(first.lines[1], "hit two"));

    EXPECT_TRUE("seek to next offset", lseek(fd, first.next_offset, SEEK_SET) == first.next_offset);
    struct sigmund_log_filter_result second;
    EXPECT_TRUE("second page succeeds", sigmund_log_filter_fd(fd, &opts, &second) == 0);
    EXPECT_TRUE("second page has third hit", second.line_count > 0 && strstr(second.lines[0], "hit three"));
    EXPECT_TRUE("second page has fourth hit", second.line_count > 1 && strstr(second.lines[1], "hit four"));
    sigmund_log_filter_result_free(&second);
    sigmund_log_filter_result_free(&first);
    close(fd);
}

int main(void) {
    test_literal_filter();
    test_similarity_filter();
    test_match_ring_wraps();
    test_lazy_large_file_stops_after_first_screen();
    test_next_offset_resumes_next_screen();
    if (failures) {
        fprintf(stderr, "viewer_filter_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: viewer filter engine");
    return 0;
}
