# Makefile
# Battlezone 98 Redux - Windows netcode patch
# Cross-compiles a 32-bit winmm.dll proxy using MinGW on Linux.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686
#
# Usage:
#   make          – build build/winmm.dll
#   make clean    – remove build directory

CXX     := i686-w64-mingw32-g++
WINDRES := i686-w64-mingw32-windres

SRC_DIR := src
OUT_DIR := build
# ../../shared holds reorder_core.h, shared verbatim with the Linux proxy.
SHARED_DIR := ../../shared

# ── Build identity (V4.9) ────────────────────────────────────────────────────
# Neither DLL logged a build id, and the shipped Windows prebuilt was
# deliberately a different vintage from the source, so a tester's log could not
# tell you which build produced it. Every "which build was that?" thread came
# from this. BZ_VERSION is set by hand at release; the commit and date come
# from git, with fallbacks so a source tarball with no .git still builds.
BZ_VERSION  ?= V5.0
BZ_COMMIT   ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BZ_DIRTY    := $(shell git diff --quiet 2>/dev/null || echo +dirty)
# SOURCE_DATE_EPOCH keeps this reproducible when a build system sets it.
BZ_DATE     ?= $(shell date -u -d "@$${SOURCE_DATE_EPOCH:-$$(date +%s)}" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)
BZ_BUILD_ID := $(BZ_VERSION) $(BZ_COMMIT)$(BZ_DIRTY) $(BZ_DATE)

TARGET  := $(OUT_DIR)/winmm.dll

SRCS := \
    $(SRC_DIR)/dllmain.cpp \
    $(SRC_DIR)/netcode_hooks.cpp \
    $(SRC_DIR)/winmm_proxy.cpp

OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OUT_DIR)/%.o,$(SRCS))

# 32-bit, shared library, stdcall ABI, Unicode
CXXFLAGS := \
    -m32 \
    -std=c++14 \
    -O2 \
    -Wall \
    -Wno-unused-parameter \
    -Wno-attributes \
    -D_WIN32_WINNT=0x0601 \
    -DWIN32_LEAN_AND_MEAN \
    -DUNICODE \
    -D_UNICODE \
    -I$(SRC_DIR) \
    -I$(SHARED_DIR) \
    -DBZ_BUILD_ID='"$(BZ_BUILD_ID)"'

# -static folds libwinpthread into the DLL alongside libgcc/libstdc++.
# Without it the V4.9 threading code made the DLL import libwinpthread-1.dll,
# which exists on no normal Windows machine — the game then failed to load
# winmm.dll at all ("fine on V4.8, errors on V4.9", first Windows tester).
# The import libs (ws2_32, winmm) are stubs either way; nothing else changes.
LDFLAGS := \
    -m32 \
    -shared \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -Wl,--enable-stdcall-fixup \
    -Wl,--out-implib,$(OUT_DIR)/libwinmm_proxy.a \
    $(SRC_DIR)/winmm.def

LIBS := -lws2_32 -lwinmm

.PHONY: all clean

all: $(OUT_DIR) $(TARGET)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo ""
	@echo "Build successful: $@"
	@echo "Copy build/winmm.dll to the game folder:"
	@echo "  steamapps/common/Battlezone 98 Redux/"

# Every shared header, not just the first one.  Under-declaring this meant an
# edit to net_globals.h or send_pace.h did not trigger a rebuild, so a stale
# DLL could ship carrying none of the change you just made.
SHARED_HDRS := $(wildcard $(SHARED_DIR)/*.h)

$(OUT_DIR)/%.o: $(SRC_DIR)/%.cpp $(SHARED_HDRS) | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT_DIR)
