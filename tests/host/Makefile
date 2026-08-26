# Host-side unit tests: pure logic compiled and run on macOS, no console needed.
#   make test        build + run everything
#   make clean
CC ?= cc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -O1 -std=c11

SRC = ../../app/src

TESTS = test_hls_parse test_urlopt test_resolve

all: $(TESTS)

test_hls_parse: test_hls_parse.c $(SRC)/hls_parse.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_hls_parse.c $(SRC)/hls_parse.c

test_urlopt: test_urlopt.c $(SRC)/urlopt.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_urlopt.c $(SRC)/urlopt.c

# resolve.c's interesting half is static; include it directly.
test_resolve: test_resolve.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_resolve.c

test: all
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all test clean
