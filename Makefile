# SPDX-License-Identifier: GPL-2.0

CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=

PREFIX ?= /usr
DESTDIR ?=

PROGRAM = afhs
SOURCES = src/afhs.c

.PHONY: all clean install uninstall

all: $(PROGRAM)

$(PROGRAM): $(SOURCES) include/afhs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -f $(PROGRAM)

install: $(PROGRAM)
	install -Dm755 $(PROGRAM) $(DESTDIR)$(PREFIX)/bin/$(PROGRAM)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PROGRAM)
