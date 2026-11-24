CXX      ?= clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -pedantic -g -pthread -Iinclude
# Tests also check for memory errors and undefined behavior. `make tsan`
# checks for data races instead (the two checkers can't be combined).
SANITIZE := -fsanitize=address,undefined

MAINS     := src/main.cpp src/server_main.cpp
LIB_SRCS  := $(filter-out $(MAINS),$(wildcard src/*.cpp))
HEADERS   := $(wildcard include/kvstore/*.h)
TEST_SRCS := $(wildcard tests/*.cpp)

.PHONY: all test tsan bench run server clean

all: bin/kv bin/kv-server

bin/kv: src/main.cpp $(LIB_SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -O2 src/main.cpp $(LIB_SRCS) -o $@

bin/kv-server: src/server_main.cpp $(LIB_SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -O2 src/server_main.cpp $(LIB_SRCS) -o $@

bin/kv-bench: bench/benchmark.cpp $(LIB_SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -O2 -DNDEBUG bench/benchmark.cpp $(LIB_SRCS) -o $@

bin/tests: $(TEST_SRCS) $(LIB_SRCS) $(HEADERS) $(wildcard tests/*.h)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SANITIZE) -Ithird_party $(TEST_SRCS) $(LIB_SRCS) -o $@

bin/tests-tsan: $(TEST_SRCS) $(LIB_SRCS) $(HEADERS) $(wildcard tests/*.h)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -fsanitize=thread -Ithird_party $(TEST_SRCS) $(LIB_SRCS) -o $@

test: bin/tests
	./bin/tests

# Skips the crash test, which forks a process; that doesn't mix well with
# the thread checker.
tsan: bin/tests-tsan
	./bin/tests-tsan --test-case-exclude="*killed*"

# Uses an optimized build. Run with nothing else busy for steadier numbers.
bench: bin/kv-bench
	./bin/kv-bench

run: bin/kv
	./bin/kv

server: bin/kv-server
	./bin/kv-server

clean:
	rm -rf bin
