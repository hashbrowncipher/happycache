prefix = /usr/local

CFLAGS=-O2 -g -Wall -std=gnu11
LDLIBS=-lpthread

.PHONY: all
all: happycache


.PHONY: install
install: happycache
	install -D -m 755 happycache $(DESTDIR)$(prefix)/bin/happycache
