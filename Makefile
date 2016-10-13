prefix = /usr/local

CFLAGS=-O2 -g -Wall
LDLIBS=-lpthread

.PHONY: all
all: happycache


.PHONY: install
install: happycache
	install -D -m 755 -t $(DESTDIR)$(prefix)/bin/ happycache
