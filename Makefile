# vaccel top-level build.
#   make              kernel module + user-space tools
#   make fuzz         buddy allocator fuzz test (ASan/UBSan, no root needed)
#   sudo make load    insert the module (scripts/load.sh)
#   sudo make test    run the functional test suite against the loaded module
#   sudo make unload
#   make qemu-test KERNEL=/path/to/bzImage KDIR=/path/to/matching/kernel/tree
KDIR ?= /lib/modules/$(shell uname -r)/build

all: module user

module:
	$(MAKE) -C kernel KDIR=$(KDIR)

user:
	$(MAKE) -C user

fuzz:
	$(MAKE) -C user fuzz

load: module
	./scripts/load.sh

unload:
	./scripts/unload.sh

test: user
	./user/build/test_vaccel

bench: user
	./user/build/bench_vaccel

qemu-test:
	./scripts/qemu-test.sh

clean:
	$(MAKE) -C kernel KDIR=$(KDIR) clean
	$(MAKE) -C user clean

.PHONY: all module user fuzz load unload test bench qemu-test clean
