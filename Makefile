# ============================================================
#  HFT Signal Engine — Makefile (macOS Rosetta/Intel Patch)
# ============================================================

TARGET  := hft_engine
SRC     := hft_engine.cpp
CXX     := g++

# macOS uses sysctl to count cores if nproc is missing
NPROC   := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# ── Rosetta/Intel ZeroMQ path ───────────────────────────
ZMQ_PREFIX := /usr/local
ZMQ_INC    := -I$(ZMQ_PREFIX)/include
ZMQ_LIB    := -L$(ZMQ_PREFIX)/lib -lzmq

# ── Compiler flags ────────────────────────────────────────────
CXXFLAGS_RELEASE := \
    -O3 \
    -arch x86_64 \
    -std=c++20 \
    -pthread \
    -flto=thin \
    -fno-omit-frame-pointer \
    $(ZMQ_INC) \
    -Wall -Wextra -Wno-unused-parameter

CXXFLAGS_DEBUG := \
    -O0 -g3 \
    -arch x86_64 \
    -std=c++20 \
    -pthread \
    $(ZMQ_INC) \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -Wall -Wextra

# ── Linker flags ──────────────────────────────────────────────
LDFLAGS  := -arch x86_64 -pthread -lm $(ZMQ_LIB)
LDFLAGS_DEBUG := -arch x86_64 -pthread -lm -fsanitize=address,undefined $(ZMQ_LIB)

# ── Default target ────────────────────────────────────────────
.PHONY: all release debug clean bench profile check

all: release

release: $(SRC)
	@echo "[BUILD] release  →  $(TARGET)"
	$(CXX) $(CXXFLAGS_RELEASE) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "[OK]   $(TARGET) built"

debug: $(SRC)
	@echo "[BUILD] debug  →  $(TARGET)_debug"
	$(CXX) $(CXXFLAGS_DEBUG) -o $(TARGET)_debug $(SRC) $(LDFLAGS_DEBUG)
	@echo "[OK]   $(TARGET)_debug built"

profile: $(SRC)
	@echo "[BUILD] profile  →  $(TARGET)_profile"
	$(CXX) $(CXXFLAGS_RELEASE) -pg -o $(TARGET)_profile $(SRC) $(LDFLAGS)
	@echo "[OK]   run ./$(TARGET)_profile then: gprof $(TARGET)_profile gmon.out | less"

bench: release
	@echo "[BENCH] perf stat — 5 second run"
	sudo perf stat -e cycles,instructions,cache-misses,cache-references,branch-misses \
	    timeout 5 ./$(TARGET) 2>&1 | tail -20

clean:
	@echo "[CLEAN]"
	rm -f $(TARGET) $(TARGET)_debug $(TARGET)_profile gmon.out perf.data*

check:
	@echo "=== System check ==="
	@echo -n "  g++ version:   "; $(CXX) --version | head -1
	@echo -n "  CPU cores:     "; sysctl -n hw.logicalcpu
	@echo -n "  Kernel:        "; uname -r
	@echo "=== End check ==="