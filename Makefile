CC ?= cc
CPPFLAGS ?=
EXTRA_CPPFLAGS ?=
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
STATIC_LDFLAGS ?=
else
STATIC_LDFLAGS ?= -static
endif
TEST_LDFLAGS ?=
VERSION_BASE ?= $(shell bash .github/scripts/resolve_version.sh --base 2>/dev/null || printf dev)
VERSION ?= $(shell if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git describe --tags --exact-match --dirty 2>/dev/null || printf '%s-%s%s\n' "$(VERSION_BASE)" "$$(git rev-parse --short HEAD)" "$$(git diff --quiet 2>/dev/null || echo -dirty)"; else printf '%s\n' "$(VERSION_BASE)"; fi)
VERSION_CPPFLAG := -DHOLD_VERSION=\"$(VERSION)\"

# Every translation unit. wildcard is portable in GNU make; one glob per layer
# directory (GNU make has no portable recursive **). main.c + cli.c sit directly
# under src/; the layers live in src/<layer>/.
SRCS := $(wildcard src/*.c) \
        $(wildcard src/core/*.c) \
        $(wildcard src/platform/*.c) \
        $(wildcard src/store/*.c) \
        $(wildcard src/console/*.c) \
        $(wildcard src/access/*.c) \
        $(wildcard src/runtime/*.c) \
        $(wildcard src/viewer/*.c)

# Separate object trees per build "personality" so the test objects (built with
# -DHOLD_TESTING and a different HOLD_BOOT_ID_PATH) can never be linked
# into a release binary, and vice versa.
OBJS      := $(patsubst src/%.c,obj/%.o,$(SRCS))
TEST_OBJS := $(patsubst src/%.c,obj-test/%.o,$(SRCS))

INCLUDES := -Iinclude
ALL_CPPFLAGS := $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(INCLUDES) $(VERSION_CPPFLAG)
TEST_CPPFLAGS := -DHOLD_TESTING -DHOLD_BOOT_ID_PATH='"/tmp/hold_test_boot_id"'

all: hold

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) -c -o $@ $<

hold: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $(OBJS)

hold-dynamic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o hold-dynamic $(OBJS)

obj-test/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o hold $(TEST_OBJS)
	@bash tests/test_hold.sh
	@$(MAKE) viewer-filter-test
	@bash tests/test_version_makefile.sh
	@bash tests/test_release_installer.sh

viewer-filter-test:
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o viewer-filter-test tests/viewer_filter_test.c src/viewer/filter.c src/core/logging.c src/core/json.c src/core/util.c src/platform/paths.c
	@./viewer-filter-test

test-040: hold-dynamic
	@HOLD_BIN=./hold-dynamic bash tests/test_040_dockerish.sh

print-version:
	@printf '%s\n' '$(VERSION)'

check: test

review-build:
	@bash scripts/build_review.sh

review-fixture:
	@bash scripts/create_review_fixture.sh

# Full local CI mirror (one command, same rigor as GitHub CI): static + dynamic
# -Werror builds, the regression suite, ASan/UBSan, cppcheck, and
# the layer-dependency lint. Skips (visibly) only what the local toolchain lacks.
ci:
	@bash scripts/ci.sh

# Layer dependency-direction lint, shared verbatim with the GitHub CI job.
lint:
	@bash scripts/lint_layers.sh

clean:
	rm -f hold hold-dynamic viewer-filter-test
	rm -rf obj obj-test

.PHONY: all clean test test-040 check ci lint viewer-filter-test print-version review-build review-fixture
