# Simple make based build of MiniVectorDB.
#
#   make            build build/minivectordb
#   make test       build and run the local test suite
#   make bench      build and run the performance benchmark
#   make clean      remove the build directory

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
DEPFLAGS := -MMD -MP
BUILD    ?= build
SRC_DIR  := src
OBJ_DIR  := $(BUILD)/obj
SOURCES  := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS  := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)
TARGET   := $(BUILD)/minivectordb

.PHONY: all test bench large clean

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD) $(OBJ_DIR):
	mkdir -p $@

test: $(TARGET)
	python3 tests/run_tests.py --binary $(TARGET)
	python3 tests/fuzz.py --binary $(TARGET) --iterations 400 --seed 1
	python3 tests/fuzz.py --binary $(TARGET) --mode txn --iterations 300 --seed 1
	python3 tests/fuzz.py --binary $(TARGET) --mode recover --iterations 80 --seed 1
	python3 tests/fuzz_ddl.py --binary $(TARGET) --iterations 200 --seed 1
	python3 tests/bigtree.py --binary $(TARGET) --rows 30000

bench: $(TARGET)
	python3 tests/bench.py --binary $(TARGET)

large: $(TARGET)
	python3 tests/large_data.py --binary $(TARGET) --rows 100000

clean:
	rm -rf $(BUILD)

-include $(DEPS)