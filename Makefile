# shortcuts, the real build is cmake
BUILD ?= build

.PHONY: all build test bench plots asan tsan clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build $(BUILD) -j

test: build
	./$(BUILD)/unit_tests
	./$(BUILD)/e2e_tests

# every scenario, ~5 min, then the plots
bench: build
	for s in baseline rate_sweep overload credits burst slow_consumer producer_failure faults queue_compare; do \
		python3 experiments/run.py experiments/scenarios/$$s.yaml --build-dir $(BUILD) || exit 1; \
	done
	python3 analysis/plot.py

plots:
	python3 analysis/plot.py

asan:
	cmake -S . -B build-asan -DDAQ_SANITIZE=ON
	cmake --build build-asan -j
	./build-asan/unit_tests && ./build-asan/e2e_tests

tsan:
	cmake -S . -B build-tsan -DDAQ_TSAN=ON
	cmake --build build-tsan -j
	./build-tsan/unit_tests

clean:
	rm -rf build build-asan build-tsan
