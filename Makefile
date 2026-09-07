CC      ?= aarch64-linux-android-clang
CFLAGS  := -static -O2 -s -Wall -Wextra -fdata-sections -ffunction-sections
LDFLAGS := -Wl,--gc-sections
SRC     := src/touchd.c
OUT     := bin/touchd

.PHONY: all clean check ndk zig local

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built: $@"

zig:
	zig cc -target aarch64-linux-musl $(CFLAGS) $(LDFLAGS) -o $(OUT) $(SRC)

ndk:
	$(MAKE) CC=aarch64-linux-android-clang all

local:
	$(MAKE) CC=gcc CFLAGS="-O2 -Wall -Wextra" all

check:
	$(CC) -fsyntax-only -Wall -Wextra $(SRC)

clean:
	rm -f $(OUT)
