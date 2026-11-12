# vbd - branchable, lazy-fetching, time-travel block device built on the kernel XArray

XARRAY     = deps/libxarray
XARRAY_LIB = $(XARRAY)/build/libxarray.a
URING      = deps/liburing
URING_LIB  = $(URING)/src/liburing.a

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall
CPPFLAGS = -Iinclude -I$(XARRAY)/include -I$(XARRAY)/shim -I$(XARRAY)/lib -D_LGPL_SOURCE
LDLIBS   = $(XARRAY_LIB) -lurcu -lcurl -lpthread

LIB      = build/libvbd.a
LIB_OBJS = build/volume.o build/crc32c.o build/base/file.o build/base/http.o
BIN      = build/vbd
BIN_OBJS = build/main.o build/ublk/server.o

.PHONY: all test clean $(XARRAY_LIB) $(URING_LIB)
all: $(LIB) $(BIN)

$(BIN): $(BIN_OBJS) $(LIB) $(XARRAY_LIB) $(URING_LIB)
	$(CC) $(CFLAGS) $(BIN_OBJS) $(LIB) $(URING_LIB) $(LDLIBS) -o $@

$(XARRAY_LIB):
	$(MAKE) -C $(XARRAY)

$(URING_LIB):
	cd $(URING) && { test -f config-host.mak || ./configure > /dev/null; }
	$(MAKE) -s -C $(URING)/src

# talks to the kernel, not to the XArray: system headers, not the shim
build/ublk/server.o: CPPFLAGS = -Iinclude -I$(URING)/src/include

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

build/%.o: src/%.c include/*.h | build
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/volume_test: tests/volume_test.c $(LIB) $(XARRAY_LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

test: build/volume_test $(BIN)
	$(MAKE) -C $(XARRAY) test
	./build/volume_test
	tests/cli_test.sh $(BIN)
	tests/crash_test.sh $(BIN)
	tests/http_test.sh $(BIN)
	tests/ublk_test.sh $(BIN)

build:
	mkdir -p $@

clean:
	$(MAKE) -C $(XARRAY) clean
	$(MAKE) -s -C $(URING) clean
	rm -rf build
