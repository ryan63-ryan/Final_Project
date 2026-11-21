CXX = c++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
BUILD = build-make
CORE = src/block_pool.cpp src/block_table.cpp src/radix_tree.cpp src/eviction_policy.cpp
HEADERS = $(wildcard include/kvcache/*.h)

.PHONY: all test sanitize clean
all: $(BUILD)/storage_tests $(BUILD)/radix_tests $(BUILD)/eviction_tests $(BUILD)/block_demo $(BUILD)/kvcache

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/storage_tests: tests/storage_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/radix_tests: tests/radix_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/eviction_tests: tests/eviction_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/block_demo: examples/block_demo.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/kvcache: src/main.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

test: all
	./$(BUILD)/storage_tests
	./$(BUILD)/radix_tests
	./$(BUILD)/eviction_tests
	./$(BUILD)/block_demo tests/golden/block_demo.in > $(BUILD)/demo.actual
	diff -u tests/golden/block_demo.expected $(BUILD)/demo.actual

sanitize:
	$(MAKE) BUILD=build-asan-make CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' test

clean:
	rm -rf build-make build-asan-make
