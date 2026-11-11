CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -Iinclude -pthread
SANFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Iinclude -pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer
TSANFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Iinclude -pthread -fsanitize=thread

.PHONY: all test bench asan tsan clean

all: build/spark_test build/spark_bench build/sparkctl

build/spark_test: tests/test_main.cpp include/spark/*.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) tests/test_main.cpp -o $@

build/spark_bench: benchmarks/bench.cpp include/spark/*.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) benchmarks/bench.cpp -o $@

build/sparkctl: apps/sparkctl.cpp include/spark/*.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) apps/sparkctl.cpp -o $@

build/spark_test_asan: tests/test_main.cpp include/spark/*.hpp
	mkdir -p build
	$(CXX) $(SANFLAGS) tests/test_main.cpp -o $@

build/spark_test_tsan: tests/test_main.cpp include/spark/*.hpp
	mkdir -p build
	$(CXX) $(TSANFLAGS) tests/test_main.cpp -o $@

test: build/spark_test
	cd /workspace/spark && ./build/spark_test

bench: build/spark_bench
	cd /workspace/spark && ./build/spark_bench

asan: build/spark_test_asan
	cd /workspace/spark && ./build/spark_test_asan

tsan: build/spark_test_tsan
	cd /workspace/spark && ./build/spark_test_tsan

clean:
	rm -rf build
