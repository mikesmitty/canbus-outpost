# Host build of the LCC library tests: `make` builds and runs them.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g
LIB_DIR := ..
SRCS    := test_lcc.c $(wildcard $(LIB_DIR)/src/*.c)

test_lcc: $(SRCS) $(wildcard $(LIB_DIR)/include/lcc/*.h) $(LIB_DIR)/src/internal.h
	$(CC) $(CFLAGS) -I$(LIB_DIR)/include -o $@ $(SRCS)

.PHONY: run clean
run: test_lcc
	./test_lcc

clean:
	rm -f test_lcc

.DEFAULT_GOAL := run
