# vbd - branchable, lazy-fetching, time-travel block device built on the kernel XArray

XARRAY     = deps/libxarray
XARRAY_LIB = $(XARRAY)/build/libxarray.a

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall
CPPFLAGS = -I$(XARRAY)/include -I$(XARRAY)/shim -I$(XARRAY)/lib -D_LGPL_SOURCE
LDLIBS   = $(XARRAY_LIB) -lurcu -lpthread

.PHONY: all test clean $(XARRAY_LIB)
all: $(XARRAY_LIB)

$(XARRAY_LIB):
	$(MAKE) -C $(XARRAY)

test: $(XARRAY_LIB)
	$(MAKE) -C $(XARRAY) test

clean:
	$(MAKE) -C $(XARRAY) clean
	rm -rf build
