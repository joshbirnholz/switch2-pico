# Host unit tests for the hardware independent modules.
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g
SRC = ../core/src
OBJS = test_main.c $(SRC)/hd_rumble.c $(SRC)/s2_proto.c $(SRC)/mapping.c $(SRC)/ds5.c $(SRC)/x360.c $(SRC)/mode_select.c

run: test_runner
	./test_runner

test_runner: $(OBJS) $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $(OBJS) -lm

clean:
	rm -f test_runner

.PHONY: run clean
