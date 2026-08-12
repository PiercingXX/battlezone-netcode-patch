# Host-native tests for the shared headers.  No mingw, no game required.
#
#   make -C tests run

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter

BINS := reorder_test net_globals_test send_pace_test gov_trace_test buffer_filter_test send_dampen_test dampen_epoch_test dampen_storm_test dampen_purge_test dampen_gate_test

.PHONY: all run clean

all: $(BINS)

reorder_test: reorder_test.cpp ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ reorder_test.cpp

net_globals_test: net_globals_test.cpp ../shared/net_globals.h
	$(CXX) $(CXXFLAGS) -o $@ net_globals_test.cpp

send_pace_test: send_pace_test.cpp ../shared/send_pace.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ send_pace_test.cpp

gov_trace_test: gov_trace_test.cpp ../shared/gov_trace.h
	$(CXX) $(CXXFLAGS) -o $@ gov_trace_test.cpp

buffer_filter_test: buffer_filter_test.cpp ../shared/buffer_filter.h
	$(CXX) $(CXXFLAGS) -o $@ buffer_filter_test.cpp

send_dampen_test: send_dampen_test.cpp ../shared/send_dampen.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ send_dampen_test.cpp

dampen_epoch_test: dampen_epoch_test.cpp ../shared/send_dampen.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ dampen_epoch_test.cpp

dampen_storm_test: dampen_storm_test.cpp ../shared/send_dampen.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ dampen_storm_test.cpp

dampen_purge_test: dampen_purge_test.cpp ../shared/send_dampen.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ dampen_purge_test.cpp

dampen_gate_test: dampen_gate_test.cpp ../shared/send_dampen.h ../shared/reorder_core.h
	$(CXX) $(CXXFLAGS) -o $@ dampen_gate_test.cpp

run: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; echo; done; exit $$fail

clean:
	rm -f $(BINS)
