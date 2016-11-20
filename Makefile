prefix = /usr/local

CFLAGS=-O2 -g -Wall -std=gnu11
LDLIBS=-lpthread -lz

.PHONY: all
all: happycache

happycache: list.c dumping.c happycache.c

.PHONY: install
install: happycache
	install -D -m 755 happycache $(DESTDIR)$(prefix)/bin/happycache
