PREFIX  = /usr/local

INC_DIR = $(PREFIX)/include
LIB_DIR = $(PREFIX)/lib

CC      = gcc
CFLAGS  = -O0 -g -Wall -Wextra -Wno-unused-parameter -I$(INC_DIR)
LDFLAGS = -L$(LIB_DIR) -lir -lcapstone

EXAMPLE_EXES = mandelbrot 0001-basic 0002-while 0003-pointer 0004-func 0005-basic-runner-func

all: $(EXAMPLE_EXES)

$(EXAMPLE_EXES): $(notdir %): $(notdir %.c)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(EXAMPLE_EXES)
