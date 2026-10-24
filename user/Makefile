# User-space library, tools, tests and benchmark.
#   make            build everything into build/
#   make STATIC=1   static binaries (used for the QEMU test image)
#   make fuzz       build and run the buddy allocator fuzz test with ASan/UBSan
CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -I../include/uapi -pthread
LDFLAGS  += -pthread
ifeq ($(STATIC),1)
LDFLAGS  += -static
endif

B   := build
LIB := $(B)/libvaccel.a

all: $(B)/test_vaccel $(B)/bench_vaccel $(B)/vaccelctl $(B)/buddy_fuzz

$(B):
	mkdir -p $@

$(B)/vaccel.o: src/vaccel.cpp include/vaccel/vaccel.hpp ../include/uapi/vaccel.h | $(B)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(B)/vaccel.o
	$(AR) rcs $@ $^

$(B)/test_vaccel: tests/test_vaccel.cpp $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(B)/bench_vaccel: bench/bench_vaccel.cpp $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(B)/vaccelctl: tools/vaccelctl.cpp $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) $(LDFLAGS) -o $@

# The exact kernel source file, compiled for user space under sanitizers.
$(B)/buddy_fuzz: tests/buddy_fuzz.c ../kernel/vaccel_buddy.c ../kernel/vaccel_buddy.h | $(B)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=all tests/buddy_fuzz.c ../kernel/vaccel_buddy.c -o $@

fuzz: $(B)/buddy_fuzz
	./$(B)/buddy_fuzz

clean:
	rm -rf $(B)

.PHONY: all fuzz clean
