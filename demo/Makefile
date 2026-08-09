# Standalone demo — consumes INSTALLED InduRTDB via pkg-config
# Prereq: InduRTDB installed (cmake --install . --prefix <prefix>)
# Usage:  make INDU_PREFIX=<prefix> && make INDU_PREFIX=<prefix> run
INDU_PREFIX ?= /usr/local
export PKG_CONFIG_PATH := $(INDU_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)

CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 $(shell PKG_CONFIG_PATH=$(INDU_PREFIX)/lib/pkgconfig pkg-config --cflags indurtdb)
LDLIBS  = $(shell PKG_CONFIG_PATH=$(INDU_PREFIX)/lib/pkgconfig pkg-config --libs indurtdb)

all: demo

demo: demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: demo
	./demo

clean:
	rm -f demo

.PHONY: all run clean
