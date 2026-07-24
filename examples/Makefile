# Standalone Makefile for InduRTDB examples
# Usage: make && ./basic_example

CC       = gcc
CFLAGS   = -std=gnu11 -Wall -Wextra -O2
INCDIR   = -I../include
LIBDIR   = -L../build
LDLIBS   = -lindurtdb -lrt -lpthread

all: basic_example

basic_example: basic_example.c
	$(CC) $(CFLAGS) $(INCDIR) -o $@ $< $(LIBDIR) $(LDLIBS)

clean:
	rm -f basic_example

run: all
	./basic_example

.PHONY: all clean run
