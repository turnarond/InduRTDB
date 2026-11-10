# Standalone Makefile for InduRTDB examples
# Usage: make && ./basic_example

CC       = gcc
CFLAGS   = -std=gnu11 -Wall -Wextra -O2
INCDIR   = -I../include -I../client/include
LIBDIR   = -L../build
LDLIBS   = -lindurtdb -lrt -lpthread

all: basic_example peek_lifetime_example peers_example

basic_example: basic_example.c
	$(CC) $(CFLAGS) $(INCDIR) -o $@ $< $(LIBDIR) $(LDLIBS)

peek_lifetime_example: peek_lifetime_example.c
	$(CC) $(CFLAGS) $(INCDIR) -o $@ $< $(LIBDIR) $(LDLIBS)

# 成员管控示例：链 irtcli（客户端库），自包含启动 ../build/rtdbd/rtdbd
peers_example: peers_example.c
	$(CC) $(CFLAGS) $(INCDIR) -DRTDBD_BIN='"../build/rtdbd/rtdbd"' -o $@ $< -L../build/client -lirtcli -lpthread

clean:
	rm -f basic_example peek_lifetime_example peers_example

run: all
	./basic_example

run-peers: peers_example
	./peers_example

.PHONY: all clean run run-peers
