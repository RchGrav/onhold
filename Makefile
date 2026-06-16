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
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_CPPFLAG := -DSIGMUND_VERSION=\"$(VERSION)\"

all: sigmund

sigmund: src/sigmund.c
	$(CC) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS) $(VERSION_CPPFLAG) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $<

sigmund-dynamic: src/sigmund.c
	$(CC) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS) $(VERSION_CPPFLAG) $(LDFLAGS) -o sigmund-dynamic $<

clean:
	rm -f sigmund sigmund-dynamic

.PHONY: all clean test

test:
	$(CC) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS) $(VERSION_CPPFLAG) $(LDFLAGS) $(TEST_LDFLAGS) -DSIGMUND_TESTING -DSIGMUND_BOOT_ID_PATH='"/tmp/sigmund_test_boot_id"' -o sigmund src/sigmund.c
	@bash tests/test_sigmund.sh
