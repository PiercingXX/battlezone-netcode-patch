CC := i686-w64-mingw32-g++
CFLAGS := -O2 -s -Wall -Wextra -Wpedantic -shared -static-libgcc -static-libstdc++
LDFLAGS := -Wl,--subsystem,windows -Wl,--out-implib,build/libdsound_proxy.a
LIBS := -lws2_32
SRC := src/dsound_proxy.cpp
DEF := src/dsound_proxy.def
OUT := build/dsound.dll

.PHONY: all clean

all: $(OUT)

build:
	mkdir -p build

$(OUT): $(SRC) $(DEF) | build
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(DEF) $(LDFLAGS) $(LIBS)

clean:
	rm -rf build