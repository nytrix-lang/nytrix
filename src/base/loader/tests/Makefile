ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
BUILD ?= $(ROOT)/build
OUT := $(BUILD)/loader-test
CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -I$(ROOT)/src
NY_FULL ?= $(BUILD)/release/ny-full
LDLIBS := -L/usr/lib/llvm21/lib -lLLVM -lclang -lm -lpthread -lz3 \
          -lcurl -fopenmp -lomp -L$(BUILD)/release -lnytrixrt \
          -Wl,-rpath,$(BUILD)/release
OBJDIR := $(BUILD)/release/CMakeFiles/nytrix_compiler.dir
OBJECTS = $(shell for o in $$(find $(OBJDIR) -name '*.o' 2>/dev/null); do \
  v=$${o#$(OBJDIR)/}; src=$(ROOT)/$${v%.o}; \
  [ -f "$$src" ] && printf '%s\n' "$$o"; done)

.PHONY: all run debug-path clean
all: $(OUT)/module_identity

$(OUT):
	mkdir -p $@

$(OUT)/module_identity: module_identity.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(OBJECTS) $(LDLIBS)

run: $(OUT)/module_identity debug-path
	$<

debug-path: | $(OUT)
	NYTRIX_STD_CACHE=0 $(NY_FULL) -g -O0 \
		--emit-ir=$(OUT)/stdlib-debug.ll -emit-only stdlib-debug-path.ny
	grep -F 'DIFile(filename: "str.ny", directory: "$(ROOT)/lib")' \
		$(OUT)/stdlib-debug.ll >/dev/null
	! grep -E 'DIFile\(filename: "<(stdlib|repl_std)>"' \
		$(OUT)/stdlib-debug.ll

clean:
	rm -rf $(OUT)
