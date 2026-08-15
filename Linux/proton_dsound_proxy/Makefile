CC := i686-w64-mingw32-g++
# ../../shared holds reorder_core.h, shared verbatim with the Windows proxy.
SHARED := ../../shared
# ── Build identity (V4.9) ────────────────────────────────────────────────────
# Neither DLL logged a build id, and the shipped Windows prebuilt was
# deliberately a different vintage from the source, so a tester's log could not
# tell you which build produced it. Every "which build was that?" thread came
# from this. BZ_VERSION is set by hand at release; the commit and date come
# from git, with fallbacks so a source tarball with no .git still builds.
BZ_VERSION  ?= V5.1
BZ_COMMIT   ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
BZ_DIRTY    := $(shell git diff --quiet 2>/dev/null || echo +dirty)
# SOURCE_DATE_EPOCH keeps this reproducible when a build system sets it.
BZ_DATE     ?= $(shell date -u -d "@$${SOURCE_DATE_EPOCH:-$$(date +%s)}" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)
BZ_BUILD_ID := $(BZ_VERSION) $(BZ_COMMIT)$(BZ_DIRTY) $(BZ_DATE)

CFLAGS := -O2 -s -Wall -Wextra -Wpedantic -shared -static-libgcc -static-libstdc++ -I$(SHARED) -DBZ_BUILD_ID='"$(BZ_BUILD_ID)"'
LDFLAGS := -Wl,--subsystem,windows -Wl,--out-implib,build/libdsound_proxy.a
LIBS := -lws2_32
SRC := src/dsound_proxy.cpp
DEF := src/dsound_proxy.def
# Every shared header, not just the first one.  Under-declaring this meant an
# edit to net_globals.h or send_pace.h did not trigger a rebuild, so a stale
# DLL could ship carrying none of the change you just made.
HDRS := $(wildcard $(SHARED)/*.h)
OUT := build/dsound.dll

.PHONY: all clean

all: $(OUT)

build:
	mkdir -p build

$(OUT): $(SRC) $(DEF) $(HDRS) | build
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(DEF) $(LDFLAGS) $(LIBS)

clean:
	rm -rf build