CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -Iinclude -Isrc
LDFLAGS  += -pthread

SRCS := $(wildcard src/*.cc)
OBJS := $(SRCS:src/%.cc=build/%.o)

.PHONY: all test bench clean asan tsan

all: build/liblsmkv.a build/lsmkv_test build/lsmkv_bench build/lsmkv_cli

build/%.o: src/%.cc $(wildcard src/*.h include/lsmkv/*.h)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/liblsmkv.a: $(OBJS)
	ar rcs $@ $^

build/lsmkv_test: tests/test_main.cc build/liblsmkv.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/liblsmkv.a -o $@ $(LDFLAGS)

build/lsmkv_bench: tools/bench.cc build/liblsmkv.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/liblsmkv.a -o $@ $(LDFLAGS)

build/lsmkv_cli: tools/cli.cc build/liblsmkv.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/liblsmkv.a -o $@ $(LDFLAGS)

test: build/lsmkv_test
	./build/lsmkv_test

bench: build/lsmkv_bench
	./build/lsmkv_bench

# Sanitizer builds: `make asan` / `make tsan` rebuild everything instrumented.
asan:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-pthread -fsanitize=address,undefined"

tsan:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS="-std=c++17 -O1 -g -fsanitize=thread" LDFLAGS="-pthread -fsanitize=thread"

clean:
	rm -rf build
