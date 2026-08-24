CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Iinclude
OPT       = -O2
BENCHOPT  = -O3 -DNDEBUG

BUILD = build

.PHONY: all test stress bench clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_scenarios: tests/test_scenarios.cpp tests/harness.hpp include/matcheng/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OPT) tests/test_scenarios.cpp -o $@

$(BUILD)/test_stress: tests/test_stress.cpp tests/harness.hpp include/matcheng/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OPT) tests/test_stress.cpp -o $@

$(BUILD)/bench: bench/bench.cpp include/matcheng/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(BENCHOPT) bench/bench.cpp -o $@

test: $(BUILD)/test_scenarios $(BUILD)/test_stress
	$(BUILD)/test_scenarios
	$(BUILD)/test_stress

stress: $(BUILD)/test_stress
	$(BUILD)/test_stress

bench: $(BUILD)/bench
	$(BUILD)/bench

clean:
	rm -rf $(BUILD)
