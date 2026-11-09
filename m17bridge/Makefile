CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
# The KISS and M17-packet framing code is the station's, not a copy of it: this
# tool is built out of the same sources m17web uses, so a change to the framing
# cannot leave the two disagreeing.
STATION := ../m17gismo
CPPFLAGS += -Isrc -I$(STATION)

BINS := tests/test_kiss tests/test_m17_packet tests/test_m17_bridge
TOOL := tools/m17bridge

.PHONY: all test itest san clean tool

all: test tool

test: $(BINS)
	@./tests/test_kiss
	@./tests/test_m17_packet
	@./tests/test_m17_bridge

tests/test_kiss: $(STATION)/kiss.c tests/test_kiss.c $(STATION)/kiss.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(STATION)/kiss.c tests/test_kiss.c

tests/test_m17_packet: $(STATION)/m17_packet.c tests/test_m17_packet.c $(STATION)/m17_packet.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(STATION)/m17_packet.c tests/test_m17_packet.c

tests/test_m17_bridge: src/m17_bridge.c tests/test_m17_bridge.c src/m17_bridge.h $(STATION)/m17_packet.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/m17_bridge.c tests/test_m17_bridge.c

tool: $(TOOL)

$(TOOL): tools/m17bridge.c $(STATION)/kiss.c $(STATION)/m17_packet.c src/m17_bridge.c $(STATION)/kiss.h $(STATION)/m17_packet.h src/m17_bridge.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tools/m17bridge.c $(STATION)/kiss.c $(STATION)/m17_packet.c src/m17_bridge.c

# The loopback test starts the tool, connects two KISS clients and checks that
# frames cross intact and that the console commands work.
itest: $(TOOL)
	@python3 tests/test_loopback.py

# Same tests under ASan/UBSan, to catch buffer arithmetic mistakes in the
# escape, chunking and reassembly paths that a passing assertion would hide.
san: CFLAGS := -std=c11 -g -O1 -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
san: clean test

clean:
	rm -f $(BINS) $(TOOL)
