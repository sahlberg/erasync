# Copyright (C) 2026 Ronnie Sahlberg. SPDX-License-Identifier: BSD-2-Clause
CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror -g -O2
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
SRCS = main.c sync.c sender.c receiver.c era.c dm.c record.c ranges.c \
       common.c fsutil.c proto.c remote.c config.c
OBJS = $(SRCS:.c=.o)

.PHONY: all check install uninstall clean

all: erasync

erasync: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(OBJS): $(wildcard *.h)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/ranges_test: tests/ranges_test.c ranges.c common.c ranges.h common.h
	$(CC) $(CFLAGS) -o $@ tests/ranges_test.c ranges.c common.c

# Everything but the range test needs root: tests/era.sh builds dm-era
# devices on loop files, and skips when it cannot.
check: erasync tests/ranges_test
	./tests/ranges_test
	./tests/era.sh || [ $$? = 77 ]

install: erasync
	install -D -m 755 erasync $(DESTDIR)$(BINDIR)/erasync
	install -D -m 644 erasync.1 $(DESTDIR)$(MANDIR)/man1/erasync.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/erasync $(DESTDIR)$(MANDIR)/man1/erasync.1

clean:
	rm -f $(OBJS) erasync tests/ranges_test
