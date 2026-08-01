# Standalone demo — uses InduRTDB as external lib
# Build: make
#   (requires libindurtdb.a built first: cd .. && mkdir build && cd build && cmake .. && make)

INDU_DIR = ..
CC       = gcc
CFLAGS   = -std=gnu11 -Wall -Wextra -O2 -I$(INDU_DIR)/include
LDLIBS   = -L$(INDU_DIR)/build -lindurtdb -lrt -lpthread

all: demo

demo: demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: demo
	./demo

clean:
	rm -f demo

.PHONY: all run clean
