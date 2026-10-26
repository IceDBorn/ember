CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Isrc -DUNICODE -D_UNICODE
LDFLAGS = -shared -static -s -Wl,--exclude-all-symbols

SRC = $(wildcard src/*.c) src/affinity/affinity_fix.c
HEADERS = $(wildcard src/*.h src/affinity/*.h)

all: ember.dll

ember.dll: $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) -lkernel32

clean:
	rm -f ember.dll

.PHONY: all clean
