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
    -I$(SRC_DIR)

LDFLAGS := \
    -m32 \
    -shared \
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

$(OUT_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT_DIR)
