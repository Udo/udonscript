# UdonScript Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -Isrc
LDFLAGS =

# Directories
SRC_DIR = src
CORE_DIR = $(SRC_DIR)/core
PROGRAMS_DIR = $(SRC_DIR)/programs
BIN_DIR = bin
TMP_DIR = tmp

# Core library sources
CORE_SOURCES = $(CORE_DIR)/types.cpp \
               $(CORE_DIR)/udonscript.cpp \
               $(CORE_DIR)/udonscript-builtins.cpp \
               $(CORE_DIR)/udonscript-helpers.cpp

# Core library objects
CORE_OBJECTS = $(patsubst $(CORE_DIR)/%.cpp,$(TMP_DIR)/%.o,$(CORE_SOURCES))

# Find all program sources
PROGRAM_SOURCES = $(wildcard $(PROGRAMS_DIR)/*.cpp)
PROGRAMS = $(patsubst $(PROGRAMS_DIR)/%.cpp,$(BIN_DIR)/%,$(PROGRAM_SOURCES))

# Default target
.PHONY: all
all: directories $(PROGRAMS)

# Create necessary directories
.PHONY: directories
directories:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(TMP_DIR)

# Compile core library objects
$(TMP_DIR)/%.o: $(CORE_DIR)/%.cpp
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Link programs
$(BIN_DIR)/%: $(PROGRAMS_DIR)/%.cpp $(CORE_OBJECTS)
	@echo "Building $@..."
	@$(CXX) $(CXXFLAGS) $< $(CORE_OBJECTS) -o $@ $(LDFLAGS)

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(TMP_DIR)/*.o
	@rm -rf $(BIN_DIR)/*
	@rm -rf build_cmake

# Run hello world
.PHONY: run
run: all
	@echo "Running helloworld..."
	@$(BIN_DIR)/helloworld

.PHONY: help
help:
	@echo "UdonScript Build System"
	@echo "======================="
	@echo "Targets:"
	@echo "  all       - Build all programs (default)"
	@echo "  clean     - Remove all build artifacts"
	@echo "  run       - Build and run helloworld"
	@echo "  help      - Show this help message"
