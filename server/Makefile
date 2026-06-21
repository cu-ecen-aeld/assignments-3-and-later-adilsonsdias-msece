#CROSS_COMPILE ?=aarch64-none-linux-gnu-
CROSS_COMPILE ?=
ifeq ($(CROSS_COMPILE),)
CC = gcc
else
CC = $(CROSS_COMPILE)gcc
endif

CFLAGS ?= -Wall -Wextra -g
LDFLAGS = -pthread -lrt

TARGET = aesdsocket
SRC = aesdsocket.c
OBJS = $(SRC:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) *.o
