# Optcheck driver suite: standalone C regression binaries for the NYIR
# optimizer passes in src/code/ir/opt/.
#
# Every <opt>.c here builds one self-contained binary that constructs NYIR
# functions by hand, runs the pass under test, and demands verifier-clean
# output with unchanged evaluation results (see util.h).
#
# Usage (from this directory):
#   make                  build every driver into $(ROOT)/build/driver/
#   make run              build + run every driver
#   make run <opt> [...]  build + run selected drivers
#   make <opt>            shorthand for a single driver
#   make clean            remove built binaries
#   make ASAN=1 run       same, but link against the ASan library build
#                         (cmake -B build/asan -DCMAKE_BUILD_TYPE=Debug
#                          -DCMAKE_C_FLAGS="-fsanitize=address -g -O1"
#                          first) to catch the memory-safety regressions
#                          several of these drivers exist to guard.
#
# Drivers link every object of the nytrix_compiler library; rebuild it
# with `./make ny` (or let this Makefile do it once) after touching the
# optimizer sources.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST))))/../../../../..)
BUILD ?= $(ROOT)/build
OUT := $(BUILD)/driver

ifeq ($(ASAN),1)
FLAVOR := asan
SANFLAGS := -fsanitize=address
else
FLAVOR := release
SANFLAGS :=
endif

CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -I$(ROOT)/src
LDLIBS := -L/usr/lib/llvm21/lib -lLLVM -lclang -lm -lpthread -lz3 -lgmp \
          -lcurl -fopenmp -lomp \
          -L$(BUILD)/release -lnytrixrt -Wl,-rpath,$(BUILD)/release

OBJDIR := $(BUILD)/$(FLAVOR)/CMakeFiles/nytrix_compiler.dir
# Orphaned object from a removed source file; harmless to skip.
# Link only objects whose source still exists under $(ROOT)/src.  Stale
# objects from the directory reorganization (old and moved translations both
# compiled, e.g. the previous repl layout) would otherwise duplicate symbols
# at link time.
LIVE_OBJECTS = $(shell for o in $$(find $(OBJDIR) -name '*.o' 2>/dev/null); do v=$${o#$(OBJDIR)/}; src=$(ROOT)/$${v%.o}; [ -f "$$src" ] && printf '%s\n' "$$o"; done)
OBJECTS := $(LIVE_OBJECTS)

DRIVER_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
OPTS := $(basename $(notdir $(wildcard $(DRIVER_DIR)/*.c)))
BINS := $(addprefix $(OUT)/,$(OPTS))

# `make run <opts...>`: goals next to `run` pick drivers; bare `make run`
# runs everything.
REQUESTED := $(filter-out all run clean help,$(MAKECMDGOALS))
SELECTED := $(if $(filter run,$(MAKECMDGOALS)),$(if $(REQUESTED),$(REQUESTED),$(OPTS)),$(REQUESTED))

.PHONY: all run clean help $(OPTS)
.DEFAULT_GOAL := all

all: $(BINS)

run: $(addsuffix .run,$(SELECTED))

%.run: $(OUT)/%
	$<

$(OPTS): %: %.run

clean:
	rm -rf $(OUT)

help:
	@sed -n '2,20p' $(firstword $(MAKEFILE_LIST))

# Library bootstrap: when no compiler objects exist yet, produce them via
# the project's own build entry point, then re-evaluate $(OBJECTS) in the
# recipes below (they always query the filesystem at link time).
library:
	@if [ -z "$(OBJECTS)" ]; then \
	  echo "== driver: building nytrix_compiler ($(FLAVOR))"; \
	  $(ROOT)/make ny >/dev/null || cmake --build $(BUILD)/$(FLAVOR) --target ny; \
	fi

$(OUT):
	mkdir -p $(OUT)

LINK_OBJECTS = $(LIVE_OBJECTS)

$(BINS): $(OUT)/%: $(DRIVER_DIR)/%.c util.h $(OBJECTS) | $(OUT) library
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ $< $(LINK_OBJECTS) $(LDLIBS)
