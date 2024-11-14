GPGNET ?= gpgnet-mock
PROXY ?= proxy
CFLAGS = -std=gnu11 -O2 -W -Wall -Wextra -g -I. -Werror
CFLAGS_MONGOOSE += -DMG_ENABLE_LINES

ifeq ($(OS),Windows_NT)
  GPGNET := $(GPGNET).exe
  PROXY := $(PROXY).exe
  CFLAGS += -lws2_32            # Link against Winsock library
endif

$(GPGNET): gpgnet-mock.c mongoose.c
	gcc --static $^ $(CFLAGS) $(CFLAGS_MONGOOSE) -o $(GPGNET)

$(PROXY): main.c mongoose.c
	gcc --static $^ $(CFLAGS) $(CFLAGS_MONGOOSE) -o $@

.PHONY: all test

all: $(GPGNET) $(PROXY)

test: $(GPGNET)
	$(GPGNET) --record log.csv
