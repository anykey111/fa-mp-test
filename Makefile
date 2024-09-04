GPGNET ?= gpgnet-mock
CFLAGS = -std=gnu11 -W -Wall -Wextra -g -I. -Werror
CFLAGS_MONGOOSE += -DMG_ENABLE_LINES

ifeq ($(OS),Windows_NT)
  GPGNET := $(GPGNET).exe
  CFLAGS += -lws2_32            # Link against Winsock library
endif

$(GPGNET): gpgnet-mock.c mongoose.c
	gcc $^ $(CFLAGS) $(CFLAGS_MONGOOSE) -o $(GPGNET)

.PHONY: all test

all: $(GPGNET)
	$(GPGNET)

test: $(GPGNET)
	$(GPGNET) --record log.csv
