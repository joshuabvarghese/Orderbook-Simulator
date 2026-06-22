# ─────────────────────────────────────────────────────────────────────────────
# Makefile — cross-platform (Linux x86-64 + macOS Apple Silicon / Intel)
# ─────────────────────────────────────────────────────────────────────────────

CXX      := $(shell command -v clang++ 2>/dev/null || echo g++)
STD      := -std=c++17
INCLUDES := -Iinclude

# Detect OS
UNAME := $(shell uname -s)
ARCH  := $(shell uname -m)

# Warnings — identical on both platforms
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion

# Platform-specific flags
ifeq ($(UNAME), Darwin)
    # macOS: -lpthread not needed (POSIX is in libc), no -funroll-loops on AppleClang
    LIBS      :=
    TUNE      := -O3 -march=native
    SAN_FLAGS := -fsanitize=address,undefined
else
    # Linux
    LIBS      := -lpthread
    TUNE      := -O3 -march=native -funroll-loops
    SAN_FLAGS := -fsanitize=address,undefined
endif

REL_FLAGS := $(STD) $(TUNE) $(WARNINGS)
DBG_FLAGS := $(STD) -O1 -g $(SAN_FLAGS) -Wall -Wextra

TARGET   := orderbook
TEST_BIN := run_tests
SRC      := src/main.cpp
TEST_SRC := tests/test_main.cpp
HEADERS  := $(wildcard include/*.hpp)

.PHONY: all clean run bench test sanitize info

# ── Default: release build ───────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CXX) $(REL_FLAGS) $(INCLUDES) $(SRC) -o $(TARGET) $(LIBS)
	@echo ""
	@echo "  ✓ Build successful  →  ./$(TARGET)"
	@echo "  Run:  make run        (5M messages)"
	@echo "  Run:  make bench      (1M messages — quick)"
	@echo "  Test: make test"
	@echo ""

# ── Benchmarks ───────────────────────────────────────────────────────────────
run: $(TARGET)
	./$(TARGET) 5000000

bench: $(TARGET)
	./$(TARGET) 1000000

# ── Unit tests (release) ─────────────────────────────────────────────────────
test: $(TEST_SRC) $(HEADERS)
	$(CXX) $(REL_FLAGS) $(INCLUDES) $(TEST_SRC) -o $(TEST_BIN) $(LIBS)
	./$(TEST_BIN)

# ── Unit tests + AddressSanitizer + UBSan ────────────────────────────────────
sanitize: $(TEST_SRC) $(HEADERS)
	$(CXX) $(DBG_FLAGS) $(INCLUDES) $(TEST_SRC) -o $(TEST_BIN) $(LIBS)
	./$(TEST_BIN)

# ── Show detected environment ────────────────────────────────────────────────
info:
	@echo "  OS   : $(UNAME)"
	@echo "  Arch : $(ARCH)"
	@echo "  CXX  : $(CXX)"
	@echo "  Flags: $(REL_FLAGS)"

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET) $(TEST_BIN)
