CC ?= cc
CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -pthread -lm
DISK_SOURCES := src/knurl.c
CHECK_SOURCES := src/check_main.c src/check_common.c src/check_cpu.c src/check_memory.c src/check_network.c src/check_disk.c

.PHONY: all clean test
all: knurl knurl-check
knurl: $(DISK_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DISK_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
knurl-check: $(CHECK_SOURCES) include/knurl_check.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CHECK_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
test: all
	python3 -m unittest discover -s tests -v
clean:
	rm -f knurl knurl-check
