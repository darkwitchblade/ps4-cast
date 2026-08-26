# Host-side unit tests: pure logic compiled and run on macOS, no console needed.
#   make test        build + run everything
#   make clean
CC ?= cc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -O1 -std=c11

SRC = ../../app/src

TESTS = test_hls_parse test_urlopt test_resolve test_qr

all: $(TESTS)

test_hls_parse: test_hls_parse.c $(SRC)/hls_parse.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_hls_parse.c $(SRC)/hls_parse.c

test_urlopt: test_urlopt.c $(SRC)/urlopt.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_urlopt.c $(SRC)/urlopt.c

# resolve.c's interesting half is static; include it directly.
test_resolve: test_resolve.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_resolve.c

test_qr: test_qr.c $(SRC)/qr.c $(SRC)/qrcodegen.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_qr.c $(SRC)/qr.c $(SRC)/qrcodegen.c

test: all
	@for t in $(TESTS); do ./$$t || exit 1; done
	@./test_qr > /tmp/qr_now.txt
	@golden=./test_qr_golden.txt; \
	 [ -f "$$golden" ] || golden=../../tests/host/test_qr_golden.txt; \
	 cmp -s /tmp/qr_now.txt "$$golden" && echo "test_qr: goldens match" || { echo "test_qr: GOLDEN MISMATCH"; diff "$$golden" /tmp/qr_now.txt | head; exit 1; }

clean:
	rm -f $(TESTS)

.PHONY: all test clean
