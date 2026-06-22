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
VERSION ?= $(shell git describe --tags --exact-match 2>/dev/null || printf '%s-%s%s\n' "$(VERSION_BASE)" "$$(git rev-parse --short HEAD 2>/dev/null || echo dev)" "$$(git diff --quiet 2>/dev/null || echo -dirty)")
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

all: sigmund

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) -c -o $@ $<

sigmund: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $(OBJS)

sigmund-dynamic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o sigmund-dynamic $(OBJS)

obj-test/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o sigmund $(TEST_OBJS)
	@bash tests/test_sigmund.sh
	@$(MAKE) hash-vector

# Guards the profile-hash capability key against accidental framing changes.
hash-vector:
	$(CC) $(ALL_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o hash-vector tests/profile_hash_vector.c $(HASH_VECTOR_SRCS)
	@./hash-vector

check: test

clean:
	rm -f sigmund sigmund-dynamic hash-vector
	rm -rf obj obj-test

.PHONY: all clean test check hash-vector
