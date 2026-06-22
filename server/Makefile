CROSS_COMPILE ?=

ifeq ($(CROSS_COMPILE),)
CC = gcc
else
CC = $(CROSS_COMPILE)gcc
endif

CFLAGS ?= -Wall -Wextra -g
LDFLAGS ?= -pthread

USE_AESD_CHAR_DEVICE ?= 1
CFLAGS += -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)
CFLAGS += -I../aesd-char-driver

TARGET = aesdsocket
SRC = aesdsocket.c
OBJS = $(SRC:.c=.o)

.PHONY: all clean default

all: $(TARGET)

default: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) *.o
