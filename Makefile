CXX = c++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -Iinclude
BUILD = build-make
CORE = src/block_pool.cpp src/block_table.cpp src/radix_tree.cpp src/eviction_policy.cpp
HEADERS = $(wildcard include/kvcache/*.h)

.PHONY: all test sanitize clean
all: $(BUILD)/storage_tests $(BUILD)/radix_tests $(BUILD)/eviction_tests $(BUILD)/radix_fault_tests $(BUILD)/block_demo $(BUILD)/kvcache

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/storage_tests: tests/storage_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/radix_tests: tests/radix_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/eviction_tests: tests/eviction_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/radix_fault_tests: tests/radix_fault_tests.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/block_demo: examples/block_demo.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $< -o $@

$(BUILD)/kvcache: src/main.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(DRIVER) $< -o $@

test: all
	./$(BUILD)/storage_tests
	./$(BUILD)/radix_tests
	./$(BUILD)/eviction_tests
	./$(BUILD)/radix_fault_tests
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

.PHONY: reference-test
all: $(BUILD)/reference_tests
test: reference-test

$(BUILD)/reference_tests: tests/reference_tests.cpp tests/reference_cache.cpp tests/reference_cache.h src/trace.cpp $(MANAGER) $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(MANAGER) src/trace.cpp tests/reference_cache.cpp tests/reference_tests.cpp -o $@

reference-test: $(BUILD)/reference_tests
	./$(BUILD)/reference_tests

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
# Eviction demo trace under LRU, FIFO and the sharing=off baseline.
	for c in lru fifo off; do \
	  ./$(BUILD)/kvcache --config tests/golden/eviction_$$c.cfg --trace tests/golden/eviction.trace > $(BUILD)/eviction_$$c.actual && \
	  diff -u tests/golden/eviction_$$c.expected $(BUILD)/eviction_$$c.actual || exit 1; \
	done
	./$(BUILD)/kvcache --config tests/golden/example.cfg --trace tests/golden/fork_rejected.trace > $(BUILD)/fork_rejected.actual
	diff -u tests/golden/fork_rejected.expected $(BUILD)/fork_rejected.actual

# --csv appends one row per run; needs CMake only as a script interpreter.
.PHONY: csv-test
csv-test: $(BUILD)/kvcache
	cmake -DPROGRAM=$(BUILD)/kvcache -DCONFIG=tests/golden/eviction_lru.cfg \
	  -DTRACE=tests/golden/eviction.trace -DCSV=$(BUILD)/csv_test.csv -P tests/check_csv.cmake

# Trace generator: valid, deterministic traces that the driver replays without errors.
.PHONY: gen-test
gen-test: $(BUILD)/kvcache
	python3 tests/gen_trace_test.py $(BUILD)/kvcache
