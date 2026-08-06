# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.

PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')
CC ?= cc
# Auto-discover: TESTS is whatever *_test.c files currently exist, so each user
# story's test is picked up as it lands.
TESTS := $(patsubst %_test.c,%,$(wildcard *_test.c))
CFLAGS ?= -O1 -g
CFLAGS += -std=gnu11 -D_GNU_SOURCE -I.. -Iunity -Wall -Wextra
# Keep Unity from pulling <math.h>: its C23 narrowing fmul()/fadd()/… collide with
# the static fmul()/… in the vendored curve25519-hacl64.h when a test includes both.
CFLAGS += -DUNITY_EXCLUDE_MATH_H
ifneq ($(wildcard ../uapi/$(PLATFORM)/.),)
CFLAGS += -isystem ../uapi/$(PLATFORM)
endif
ifeq ($(SANITIZE),address)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
endif
ifeq ($(SANITIZE),memory)
CC := clang
CFLAGS += -fsanitize=memory -fsanitize-memory-track-origins
endif
DEFRUN := -DRUNSTATEDIR='"/var/empty"'
UAPI_RUNDIR := $(CURDIR)/.rundir

all: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t =="; ./$$t; done

# Generic recipe (fuzz-style): each *_test.c #includes the unit under test + every
# transitive .c it references, so the recipe compiles the TU + unity.c only.
%: %_test.c unity/unity.c
	$(CC) $(CFLAGS) $(DEFRUN) -o $@ $< unity/unity.c
# ipc_uapi exercises the REAL userspace UAPI path over a test-owned UNIX socket
# (test_uapi_seam.h): needs a writable RUNSTATEDIR and pthreads. This explicit rule
# overrides the generic pattern rule for the ipc_uapi target.
ipc_uapi: ipc_uapi_test.c unity/unity.c
	$(CC) $(CFLAGS) -DRUNSTATEDIR='"$(UAPI_RUNDIR)"' -o $@ $< unity/unity.c -lpthread

clean:
	$(RM) $(TESTS); $(RM) -r .rundir
.PHONY: all clean
