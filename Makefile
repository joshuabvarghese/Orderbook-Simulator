CXX      := g++
STD      := -std=c++17
INCLUDES := -Iinclude
LIBS     := -lpthread

# Release flags
REL_FLAGS := $(STD) -O3 -march=native -funroll-loops \
             -Wall -Wextra -Wpedantic -Wshadow -Wconversion

# Debug / sanitizer flags
DBG_FLAGS := $(STD) -O1 -g \
             -fsanitize=address,undefined \
             -Wall -Wextra

TARGET    := orderbook
TEST_BIN  := run_tests
SRC       := src/main.cpp
TEST_SRC  := tests/test_main.cpp
HEADERS   := $(wildcard include/*.hpp)

.PHONY: all clean run bench test sanitize

# ── Default: release build ──────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CXX) $(REL_FLAGS) $(INCLUDES) $(SRC) -o $(TARGET) $(LIBS)
	@echo ""
	@echo "  Build successful → ./$(TARGET)"
	@echo "  Run:  make run    (5M messages)"
	@echo "  Run:  make bench  (1M messages)"
	@echo "  Test: make test"
	@echo ""

# ── Benchmarks ───────────────────────────────────────────────────────────────
run: $(TARGET)
	./$(TARGET) 5000000

bench: $(TARGET)
	./$(TARGET) 1000000

# ── Unit tests (release build) ───────────────────────────────────────────────
test: $(TEST_SRC) $(HEADERS)
	$(CXX) $(REL_FLAGS) $(INCLUDES) $(TEST_SRC) -o $(TEST_BIN) $(LIBS)
	./$(TEST_BIN)

# ── Unit tests + AddressSanitizer + UBSan ────────────────────────────────────
sanitize: $(TEST_SRC) $(HEADERS)
	$(CXX) $(DBG_FLAGS) $(INCLUDES) $(TEST_SRC) -o $(TEST_BIN) $(LIBS)
	./$(TEST_BIN)

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET) $(TEST_BIN)
