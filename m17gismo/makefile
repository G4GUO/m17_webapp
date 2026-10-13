# m17web makefile
#
# Build matrices:
#   make                 web UI + Pluto + Lime + PulseAudio (needs all the libs)
#   make sim             web UI + simulated radio, no SDR libraries needed
#   make sim PULSE=0     web UI + simulated radio, no PulseAudio either
#   make console         original ncurses UI (legacy fallback)
#
# Options (override on the command line):
#   PULSE=0        no PulseAudio back end
#   PLUTO=0        no Pluto / libiio back end
#   LIME=0         no LimeSDR back end
#   SIM=1          include the simulated radio
#   CONSOLE=1      build the ncurses UI instead of the web UI
#   WEB=0          leave the web UI out
#   EMBED=1        embed www/ into the binary (default: 1 if www/ exists)
#   DEBUG=1        -O0 -g
#
CC   = gcc
CXX  = g++
TARGET = m17web

CODEC2_PREFIX ?= /usr/local
PULSE  ?= $(shell pkg-config --exists libpulse-simple 2>/dev/null && echo 1 || echo 0)
PLUTO  ?= $(shell test -f /usr/include/iio.h -o -f /usr/local/include/iio.h && echo 1 || echo 0)
LIME   ?= $(shell test -f /usr/include/limesuite/LimeSuite.h -o -f /usr/local/include/LimeSuite.h -o -f /usr/include/LimeSuite.h && echo 1 || echo 0)
SIM    ?= 0
CONSOLE ?= 0
WEB    ?= 1
EMBED  ?= $(shell test -d www && echo 1 || echo 0)

CFLAGS   = -Wall -Wextra -O3 -I$(CODEC2_PREFIX)/include -I$(CODEC2_PREFIX)/include/codec2
CXXFLAGS = $(CFLAGS) -std=c++11
LIBS     = -lcodec2 -lpthread -lm -lrt -lstdc++

ifeq ($(DEBUG),1)
CFLAGS  += -O0 -g
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
SRCS = $(wildcard *.cpp)

# Radio back ends
ifeq ($(PLUTO),1)
CFLAGS   += -DHAVE_PLUTO
CXXFLAGS += -DHAVE_PLUTO
LIBS     += -liio
else
SRCS := $(filter-out pluto.cpp,$(SRCS))
endif

ifeq ($(LIME),1)
CFLAGS   += -DHAVE_LIME
CXXFLAGS += -DHAVE_LIME
LIBS     += -lLimeSuite
else
SRCS := $(filter-out lime.cpp,$(SRCS))
endif

ifeq ($(SIM),1)
CFLAGS   += -DHAVE_SIM
CXXFLAGS += -DHAVE_SIM
else
SRCS := $(filter-out radio_sim.cpp,$(SRCS))
endif

# Audio back ends
ifeq ($(PULSE),1)
CFLAGS   += -DHAVE_PULSE
CXXFLAGS += -DHAVE_PULSE
LIBS     += -lpulse-simple
else
SRCS := $(filter-out audio_pulse.cpp,$(SRCS))
endif

# Web UI needs the C server; the console build needs ncurses instead
CSRCS =
ifeq ($(WEB),1)
CXXFLAGS += -DUI_WEB
CSRCS    += webserver.c
else
SRCS := $(filter-out webui.cpp,$(SRCS))
endif

ifeq ($(CONSOLE),1)
CXXFLAGS += -DUI_CONSOLE
LIBS     += -lncurses
else ifeq ($(WEB),0)
$(error select a UI: CONSOLE=1 or WEB=1)
else
SRCS := $(filter-out ui_console.cpp,$(SRCS))
endif

# GPIO PTT (Raspberry Pi).  Harmless elsewhere: the pins are only touched when
# a real radio back end is in use.
CFLAGS   += -DHAVE_RPI_GPIO
CXXFLAGS += -DHAVE_RPI_GPIO

# Embed the web assets
ifeq ($(EMBED),1)
CXXFLAGS  += -DEMBED_WWW
GENERATED  = www_assets.h
endif

OBJS = $(SRCS:.cpp=.o) $(CSRCS:.c=.o)
HDRS = $(wildcard *.h) $(GENERATED)

# Any header change rebuilds everything (there are only a few files)
$(OBJS): $(HDRS)

# ---------------------------------------------------------------------------
.PHONY: all sim console clean help assets

all: $(TARGET)

sim:
	$(MAKE) SIM=1 PULSE=0 PLUTO=0 LIME=0 $(TARGET)

console:
	$(MAKE) CONSOLE=1 WEB=0 $(TARGET)

$(TARGET): $(GENERATED) $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Generate the embedded asset table from www/
www_assets.h: $(wildcard www/*) tools/embed_www.py
	python3 tools/embed_www.py www > $@

assets:
	python3 tools/embed_www.py www > www_assets.h

clean:
	rm -f $(TARGET) *.o www_assets.h

help:
	@echo "make            web UI, all available radio and audio back ends"
	@echo "make sim        web UI with the simulated radio (no hardware needed)"
	@echo "make console    legacy ncurses UI"
	@echo "options: PULSE=0 PLUTO=0 LIME=0 SIM=1 CONSOLE=1 WEB=0 EMBED=0 DEBUG=1"
	@echo "run:     ./$(TARGET) -r sim -w 8080"
