# Host-native tests for the shared headers.  No mingw, no game required.
#
#   make -C tests run

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter

BINS := reorder_test net_globals_test send_pace_test

.PHONY: all run clean

all: $(BINS)

reorder_test: reorder_test.cpp ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ reorder_test.cpp

net_globals_test: net_globals_test.cpp ../shared/net_globals.h
	$(CXX) $(CXXFLAGS) -o $@ net_globals_test.cpp

send_pace_test: send_pace_test.cpp ../shared/send_pace.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ send_pace_test.cpp

run: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; echo; done; exit $$fail

clean:
	rm -f $(BINS)
