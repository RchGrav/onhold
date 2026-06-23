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
VERSION_BASE ?= $(shell sed -n '1s/[[:space:]]*$$//p' VERSION 2>/dev/null || printf dev)
VERSION ?= $(shell if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git describe --tags --exact-match 2>/dev/null || printf '%s-%s%s\n' "$(VERSION_BASE)" "$$(git rev-parse --short HEAD)" "$$(git diff --quiet 2>/dev/null || echo -dirty)"; else printf '%s\n' "$(VERSION_BASE)"; fi)
VERSION_CPPFLAG := -DSIGMUND_VERSION=\"$(VERSION)\"

# Every translation unit. wildcard is portable in GNU make; one glob per layer
# directory (GNU make has no portable recursive **). main.c + cli.c sit directly
# under src/; the layers live in src/<layer>/.
SRCS := $(wildcard src/*.c) \
        $(wildcard src/core/*.c) \
        $(wildcard src/platform/*.c) \
        $(wildcard src/store/*.c) \
        $(wildcard src/console/*.c) \
        $(wildcard src/access/*.c) \
        $(wildcard src/runtime/*.c)

# The profile-hash compatibility test links only the layers the hash depends on.
HASH_VECTOR_SRCS := $(wildcard src/core/*.c) $(wildcard src/platform/*.c) $(wildcard src/store/*.c)

# Separate object trees per build "personality" so the test objects (built with
# -DSIGMUND_TESTING and a different SIGMUND_BOOT_ID_PATH) can never be linked
# into a release binary, and vice versa.
OBJS      := $(patsubst src/%.c,obj/%.o,$(SRCS))
TEST_OBJS := $(patsubst src/%.c,obj-test/%.o,$(SRCS))

INCLUDES := -Iinclude
ALL_CPPFLAGS := $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(INCLUDES) $(VERSION_CPPFLAG)
TEST_CPPFLAGS := -DSIGMUND_TESTING -DSIGMUND_BOOT_ID_PATH='"/tmp/sigmund_test_boot_id"'

all: sigmund mund

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) -c -o $@ $<

sigmund: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $(OBJS)

sigmund-dynamic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o sigmund-dynamic $(OBJS)

mund: sigmund
	cp sigmund mund

obj-test/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o sigmund $(TEST_OBJS)
	cp sigmund mund
	@bash tests/test_sigmund.sh
	@$(MAKE) hash-vector
	@bash tests/test_version_makefile.sh
	@bash tests/test_release_installer.sh

# Guards the profile-hash capability key against accidental framing changes.
hash-vector:
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o hash-vector tests/profile_hash_vector.c $(HASH_VECTOR_SRCS)
	@./hash-vector

print-version:
	@printf '%s\n' '$(VERSION)'

check: test

# Full local CI mirror (one command, same rigor as GitHub CI): static + dynamic
# -Werror builds, the regression suite + hash-vector, ASan/UBSan, cppcheck, and
# the layer-dependency lint. Skips (visibly) only what the local toolchain lacks.
ci:
	@bash scripts/ci.sh

# Layer dependency-direction lint, shared verbatim with the GitHub CI job.
lint:
	@bash scripts/lint_layers.sh

clean:
	rm -f sigmund mund sigmund-dynamic hash-vector
	rm -rf obj obj-test

.PHONY: all clean test check ci lint hash-vector print-version
