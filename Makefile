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
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(DRIVER) $< -o $@

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

# --- Manager (Yingtao). Additive: no line above is changed. ---
# Extra prerequisites on `all` and `test` hook these targets into the rules above,
# so `make test` and `make sanitize` also build and run the manager tests.
MANAGER = src/config.cpp src/kv_cache_manager.cpp

.PHONY: manager-test
all: $(BUILD)/manager_tests
test: manager-test

$(BUILD)/manager_tests: tests/manager_tests.cpp $(MANAGER) $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(MANAGER) $< -o $@

manager-test: $(BUILD)/manager_tests
	./$(BUILD)/manager_tests

# Sources the kvcache driver needs besides main.cpp and $(CORE). The kvcache
# recipe above compiles $(DRIVER); this line makes it rebuild when they change.
DRIVER = src/trace.cpp $(MANAGER)
$(BUILD)/kvcache: $(DRIVER)

# Golden trace: the proposal example must reproduce its expected output exactly.
.PHONY: trace-test
test: trace-test
trace-test: $(BUILD)/kvcache
	./$(BUILD)/kvcache --config tests/golden/example.cfg --trace tests/golden/example.trace > $(BUILD)/example.actual
	diff -u tests/golden/example.expected $(BUILD)/example.actual
