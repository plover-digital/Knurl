CC ?= cc
CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -pthread -lm
SOURCES := $(wildcard src/*.c)

.PHONY: all clean test
all: knurl
knurl: $(SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
test: knurl
	python3 -m unittest discover -s tests -v
clean:
	rm -f knurl
