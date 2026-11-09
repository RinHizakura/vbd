# vbd - branchable, lazy-fetching, time-travel block device built on the kernel XArray

XARRAY     = deps/libxarray
XARRAY_LIB = $(XARRAY)/build/libxarray.a

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall
CPPFLAGS = -Iinclude -I$(XARRAY)/include -I$(XARRAY)/shim -I$(XARRAY)/lib -D_LGPL_SOURCE
LDLIBS   = $(XARRAY_LIB) -lurcu -lpthread

LIB      = build/libvbd.a
LIB_OBJS = build/volume.o build/crc32c.o
BIN      = build/vbd

.PHONY: all test clean $(XARRAY_LIB)
all: $(LIB) $(BIN)

$(BIN): build/main.o $(LIB) $(XARRAY_LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(XARRAY_LIB):
	$(MAKE) -C $(XARRAY)

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

build/%.o: src/%.c include/*.h | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/volume_test: tests/volume_test.c $(LIB) $(XARRAY_LIB) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

test: build/volume_test $(BIN)
	$(MAKE) -C $(XARRAY) test
	./build/volume_test
	tests/cli_test.sh $(BIN)
	tests/crash_test.sh $(BIN)

build:
	mkdir -p $@

clean:
	$(MAKE) -C $(XARRAY) clean
	rm -rf build
