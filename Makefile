# Makefile for wl_shimeji's Umbriel support plugin
#
# Build requirements:
#   - wl_shimeji headers and libwayland-shimeji-plugins.so
#   - jansson (JSON parser)

CC              ?= cc
PKG_CONFIG      ?= pkg-config
PREFIX          ?= /usr/local
BUILDDIR        ?= build
PLUGINS_OUT_DIR ?= $(BUILDDIR)/plugins

WL_SHIMEJI_INCLUDEDIR ?= $(PREFIX)/include
WL_SHIMEJI_LIBDIR     ?= $(PREFIX)/lib

TARGET := $(PLUGINS_OUT_DIR)/libumbrielsupport.so
SRCS    := src/plugin.c src/ipc.c src/state.c
OBJS    := $(SRCS:src/%.c=$(BUILDDIR)/%.o)
DEPS    := $(OBJS:.o=.d)

JANSSON_CFLAGS := $(shell $(PKG_CONFIG) --cflags jansson)
JANSSON_LIBS   := $(shell $(PKG_CONFIG) --libs jansson)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -fPIC -Isrc \
           -I$(WL_SHIMEJI_INCLUDEDIR) $(JANSSON_CFLAGS)
LDFLAGS += -shared -Wl,--no-as-needed -L$(WL_SHIMEJI_LIBDIR)
LDLIBS  += -lwayland-shimeji-plugins $(JANSSON_LIBS)

TEST_TARGET := $(BUILDDIR)/test_state

.PHONY: all clean install test integration
all: $(TARGET)

$(BUILDDIR) $(PLUGINS_OUT_DIR):
	@mkdir -p $@

$(BUILDDIR)/%.o: src/%.c Makefile | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MF $(@:.o=.d) -c $< -o $@

$(TARGET): $(OBJS) | $(PLUGINS_OUT_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TEST_TARGET): tests/test_state.c $(BUILDDIR)/state.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_state.c $(BUILDDIR)/state.o $(JANSSON_LIBS)

integration: $(TARGET)
	LD_LIBRARY_PATH=$(WL_SHIMEJI_LIBDIR):$$LD_LIBRARY_PATH python3 tests/test_plugin.py

test: $(TEST_TARGET) integration
	$(TEST_TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/lib/wl_shimeji/
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/lib/wl_shimeji/

clean:
	rm -rf $(BUILDDIR)

sinclude $(DEPS)
