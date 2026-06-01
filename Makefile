#!/usr/bin/make -f

PREFIX  ?= /usr/local
DESTDIR ?=

CXX     ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17

CARLA_DIR    = ildaeil/carla
CARLA_INCLUDES = -I$(CARLA_DIR)/source/includes -I$(CARLA_DIR)/source/backend
MODULEDIR    = $(CARLA_DIR)/build/modules/Release

CARLA_BUILD_ARGS = \
	CARLA_BACKEND_NAMESPACE=RustyVST3 \
	CAN_GENERATE_LV2_TTL=false \
	STATIC_PLUGIN_TARGET=true \
	EXTERNAL_PLUGINS=true \
	HAVE_DGL=false \
	HAVE_FFMPEG=false \
	HAVE_FLUIDSYNTH=false \
	HAVE_HYLIA=false \
	HAVE_LIBLO=false \
	HAVE_PROJECTM=false \
	HAVE_PYQT=false \
	HAVE_X11=false \
	HAVE_YSFX=false \
	HAVE_ZYN_DEPS=false \
	HAVE_ZYN_UI_DEPS=false \
	USING_JUCE=false

CARLA_STATIC_LIBS = \
	$(MODULEDIR)/carla_engine_plugin.a \
	$(MODULEDIR)/carla_plugin.a \
	$(MODULEDIR)/native-plugins.a \
	$(MODULEDIR)/audio_decoder.a \
	$(MODULEDIR)/jackbridge.a \
	$(MODULEDIR)/lilv.a \
	$(MODULEDIR)/rtmempool.a \
	$(MODULEDIR)/water.a \
	$(MODULEDIR)/water.files.a \
	$(MODULEDIR)/zita-resampler.a

CARLA_OBJS = \
	$(CARLA_DIR)/build/plugin/Release/carla-host-plugin.cpp.o

LINK_FLAGS = -ldl -pthread -lrt -lm -lsndfile

# Carla auto-detects libmagic if installed; link against it when available
LINK_FLAGS += $(shell pkg-config --libs libmagic 2>/dev/null)

.PHONY: all carla clean install

all: carla build/vst3-wrapper.so build/stom-pi-vst3-scan

carla:
	$(MAKE) $(CARLA_BUILD_ARGS) -C $(CARLA_DIR) static-plugin -j$$(nproc)

build/vst3-wrapper.so: src/wrapper.cpp carla | build
	$(CXX) -shared -fPIC $(CXXFLAGS) $(CARLA_INCLUDES) \
		-DCARLA_BACKEND_NAMESPACE=RustyVST3 \
		-o $@ $< \
		$(CARLA_OBJS) \
		-Wl,--start-group $(CARLA_STATIC_LIBS) -Wl,--end-group \
		$(LINK_FLAGS)

build/stom-pi-vst3-scan: src/scanner.cpp carla | build
	$(CXX) $(CXXFLAGS) $(CARLA_INCLUDES) \
		-DCARLA_BACKEND_NAMESPACE=RustyVST3 \
		-o $@ $< \
		$(CARLA_OBJS) \
		-Wl,--start-group $(CARLA_STATIC_LIBS) -Wl,--end-group \
		$(LINK_FLAGS)

build:
	mkdir -p build

install: all
	install -D -m 644 build/vst3-wrapper.so $(DESTDIR)$(PREFIX)/lib/stom-pi/vst3-wrapper.so
	install -D -m 755 build/stom-pi-vst3-scan $(DESTDIR)$(PREFIX)/bin/stom-pi-vst3-scan

clean:
	rm -rf build
	$(MAKE) -C $(CARLA_DIR) distclean 2>/dev/null || true
