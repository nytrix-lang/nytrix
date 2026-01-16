.DEFAULT_GOAL := all

CC ?= clang
OPT ?= 3
SAN ?= 0
PROFILE ?= 0
PREFIX ?= /usr
DESTDIR ?=
BUILD_DIR ?= build
LLVM_CONFIG ?= llvm-config

MAKEFLAGS += -j$(shell nproc) --no-print-directory

BIN_NAME := ny
BIN       := $(BUILD_DIR)/$(BIN_NAME)
BIN_DEBUG := $(BUILD_DIR)/$(BIN_NAME)_debug
BIN_LSP   := $(BUILD_DIR)/ny_lsp

C_RESET  := $(shell printf '\033[0m')
C_GRAY   := $(shell printf '\033[90m')
C_GREEN  := $(shell printf '\033[32m')
C_CYAN   := $(shell printf '\033[1;36m')

LLVM_CFLAGS  := $(shell $(LLVM_CONFIG) --cflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core native mcjit)

# Check if bear is available
BEAR := $(shell command -v bear 2>/dev/null)

SANFLAGS :=
ifeq ($(SAN),1)
SANFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
endif

PROFFLAGS :=
ifeq ($(PROFILE),1)
PROFFLAGS += -pg
endif

OPTFLAGS := -O$(OPT)

CFLAGS_BASE := -std=c11 -g -fno-omit-frame-pointer -Wall -Wextra -Wshadow -Wstrict-prototypes -Wundef -Wcast-align -Wwrite-strings -Wunused -Isrc -Isrc/base -Isrc/rt -I$(BUILD_DIR) $(LLVM_CFLAGS) -DNYTRIX_STD_PATH="\"$(PREFIX)/share/nytrix/std_bundle.ny\""
CFLAGS_DEBUG   := $(CFLAGS_BASE) -O0 -DDEBUG $(SANFLAGS) $(PROFFLAGS)
CFLAGS_RELEASE := $(CFLAGS_BASE) $(OPTFLAGS) -DNDEBUG $(SANFLAGS) $(PROFFLAGS)

LDFLAGS := $(LLVM_LDFLAGS) -lreadline -lm -rdynamic $(SANFLAGS) $(PROFFLAGS)

# Compiler Sources (Lib)
# src subdirs
SRC_COMPILER_DIRS := src/ast src/base src/lex src/code src/repl src/wire
SRC_COMPILER := $(foreach dir,$(SRC_COMPILER_DIRS),$(wildcard $(dir)/*.c))
SRC_COMPILER += src/parse/unity.c

# Runtime Sources - Use amalgamated for build speed and consistency
SRC_RUNTIME := src/rt/init.c

# Cmd Sources
SRC_CMD_NY := src/cmd/ny/main.c
SRC_CMD_LSP := src/cmd/ny-lsp/main.c

# Objects
OBJ_COMPILER_DEBUG   := $(patsubst src/%.c,$(BUILD_DIR)/compiler/debug/%.o,$(SRC_COMPILER))
OBJ_RUNTIME_DEBUG    := $(patsubst src/rt/%.c,$(BUILD_DIR)/rt/debug/%.o,$(SRC_RUNTIME))
OBJ_DEBUG            := $(OBJ_COMPILER_DEBUG) $(OBJ_RUNTIME_DEBUG)

OBJ_COMPILER_RELEASE := $(patsubst src/%.c,$(BUILD_DIR)/compiler/release/%.o,$(SRC_COMPILER))
OBJ_RUNTIME_RELEASE  := $(patsubst src/rt/%.c,$(BUILD_DIR)/rt/release/%.o,$(SRC_RUNTIME))
OBJ_RUNTIME_SHARED   := $(patsubst src/rt/%.c,$(BUILD_DIR)/rt/shared/%.o,$(SRC_RUNTIME))
OBJ_RELEASE          := $(OBJ_COMPILER_RELEASE) $(OBJ_RUNTIME_RELEASE)

OBJ_CMD_NY_DEBUG     := $(BUILD_DIR)/cmd/ny/main_debug.o
OBJ_CMD_NY_RELEASE   := $(BUILD_DIR)/cmd/ny/main.o
OBJ_CMD_LSP_RELEASE  := $(BUILD_DIR)/cmd/ny-lsp/main.o

.PHONY: all bin debug repl lsp help clean test teststd testruntime testbench bench tidy build install uninstall coverage

all: tidy bin $(BIN_LSP) $(BUILD_DIR)/std_bundle.ny $(BUILD_DIR)/libnytrixrt.so

bin: $(BIN)
debug: $(BIN_DEBUG)
lsp: $(BIN_LSP)

repl: $(BIN) $(BUILD_DIR)/std_bundle.ny
	@./$(BIN) -i $(ARGS)

help:
	@echo "\n$(C_CYAN)Nytrix Build System$(C_RESET)"
	@echo "$(C_GRAY)--------------------------------------------------$(C_RESET)"
	@echo "$(C_GREEN)make$(C_RESET)                    Build release + std bundle + runtime so"
	@echo "$(C_GREEN)make bin$(C_RESET)                Build release executable ($(BIN))"
	@echo "$(C_GREEN)make debug$(C_RESET)              Build debug executable ($(BIN_DEBUG))"
	@echo "$(C_GREEN)make repl$(C_RESET)               Run REPL (release)"
	@echo "$(C_GREEN)make test [PAT=regex]$(C_RESET)   Run all test suites"
	@echo "$(C_GREEN)make teststd$(C_RESET)            Run standard library tests"
	@echo "$(C_GREEN)make testruntime$(C_RESET)        Run runtime compiler tests"
	@echo "$(C_GREEN)make testbench$(C_RESET)          Run performance benchmarks"
	@echo "$(C_GREEN)make install$(C_RESET)            Install to $(PREFIX)"
	@echo "$(C_GREEN)make clean$(C_RESET)              Remove build artifacts"
	@echo "$(C_GREEN)make tidy$(C_RESET)               Format code using clang-format"
	@echo "$(C_GREEN)make coverage$(C_RESET)           Build with coverage flags"
	@echo ""

build:
	@mkdir -p \
		$(BUILD_DIR)/compiler/debug/ast $(BUILD_DIR)/compiler/debug/code $(BUILD_DIR)/compiler/debug/lex $(BUILD_DIR)/compiler/debug/base $(BUILD_DIR)/compiler/debug/repl $(BUILD_DIR)/compiler/debug/wire $(BUILD_DIR)/compiler/debug/parse \
		$(BUILD_DIR)/compiler/release/ast $(BUILD_DIR)/compiler/release/code $(BUILD_DIR)/compiler/release/lex $(BUILD_DIR)/compiler/release/base $(BUILD_DIR)/compiler/release/repl $(BUILD_DIR)/compiler/release/wire $(BUILD_DIR)/compiler/release/parse \
		$(BUILD_DIR)/rt/debug $(BUILD_DIR)/rt/release $(BUILD_DIR)/rt/shared \
		$(BUILD_DIR)/cmd/ny $(BUILD_DIR)/cmd/ny-lsp

$(BUILD_DIR)/std_bundle.ny: $(wildcard std/**/*.ny) etc/tools/bundle_std | build
	@python3 etc/tools/bundle_std $@

$(BUILD_DIR)/std_symbols.h: $(BUILD_DIR)/std_bundle.ny | build
	@touch $@

# Implicit headers rule logic usually handled by -MMD, but explicit for key generated file
$(BUILD_DIR)/compiler/debug/code/core.o: $(BUILD_DIR)/std_symbols.h
$(BUILD_DIR)/compiler/release/code/core.o: $(BUILD_DIR)/std_symbols.h
$(BUILD_DIR)/compiler/debug/code/expr.o: $(BUILD_DIR)/std_symbols.h
$(BUILD_DIR)/compiler/release/code/expr.o: $(BUILD_DIR)/std_symbols.h

# Compiler Lib Rules
$(BUILD_DIR)/compiler/debug/%.o: src/%.c | build
	@echo "  $(C_GRAY)CC (debug)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(BUILD_DIR)/compiler/release/%.o: src/%.c | build
	@echo "  $(C_GRAY)CC (release)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_RELEASE) -c $< -o $@

# Runtime Lib Rules
$(BUILD_DIR)/rt/debug/%.o: src/rt/%.c | build
	@echo "  $(C_GRAY)CC (debug)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(BUILD_DIR)/rt/release/%.o: src/rt/%.c | build
	@echo "  $(C_GRAY)CC (release)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(BUILD_DIR)/rt/shared/%.o: src/rt/%.c | build
	@echo "  $(C_GRAY)CC (shared)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_RELEASE) -fPIC -c $< -o $@

# Cmd Rules
$(OBJ_CMD_NY_DEBUG): src/cmd/ny/main.c | build
	@echo "  $(C_GRAY)CC (debug)$(C_RESET) $<"
	@$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(OBJ_CMD_NY_RELEASE): src/cmd/ny/main.c | build
	@echo "  $(C_GRAY)CC (release)$(C_RESET) $<"
	@$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(OBJ_CMD_LSP_RELEASE): src/cmd/ny-lsp/main.c | build
	@echo "  $(C_GRAY)CC (release)$(C_RESET) $<"
	@$(CC) $(CFLAGS_RELEASE) -c $< -o $@

# Link Rules
$(BUILD_DIR)/libnytrixrt.so: $(OBJ_RUNTIME_SHARED) | build
	@echo "  $(C_GREEN)LD (shared)$(C_RESET) $@"
	@$(CC) -shared -Wl,-soname,libnytrixrt.so -o $@ $(OBJ_RUNTIME_SHARED) -ldl -lpthread $(SANFLAGS) $(PROFFLAGS)

$(BIN_DEBUG): $(OBJ_DEBUG) $(OBJ_CMD_NY_DEBUG) | build
	@echo "  $(C_GREEN)LD (debug)$(C_RESET) $@"
	@$(CC) $(CFLAGS_DEBUG) -o $@ $(OBJ_DEBUG) $(OBJ_CMD_NY_DEBUG) $(LDFLAGS)

$(BIN): $(OBJ_RELEASE) $(OBJ_CMD_NY_RELEASE) | build
	@echo "  $(C_GREEN)LD (release)$(C_RESET) $@"
	@$(CC) $(CFLAGS_RELEASE) -o $@ $(OBJ_RELEASE) $(OBJ_CMD_NY_RELEASE) $(LDFLAGS)

$(BIN_LSP): $(OBJ_CMD_LSP_RELEASE) $(OBJ_RELEASE) | build
	@echo "  $(C_GREEN)LD (lsp)$(C_RESET) $@"
	@$(CC) $(CFLAGS_RELEASE) -o $@ $(OBJ_CMD_LSP_RELEASE) $(OBJ_RELEASE) $(LDFLAGS)

BINDIR ?= /bin
LIBDIR ?= $(PREFIX)/lib
SHAREDIR ?= $(PREFIX)/share

install: all
	@echo "  $(C_GRAY)INSTALL$(C_RESET) $(BIN_NAME) to $(DESTDIR)$(BINDIR)"
	@mkdir -p $(DESTDIR)$(BINDIR)
	@mkdir -p $(DESTDIR)$(SHAREDIR)/nytrix
	@mkdir -p $(DESTDIR)$(LIBDIR)/nytrix
	@cp $(BIN) $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@chmod 755 $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@cp $(BUILD_DIR)/std_bundle.ny $(DESTDIR)$(SHAREDIR)/nytrix/std_bundle.ny
	@chmod 644 $(DESTDIR)$(SHAREDIR)/nytrix/std_bundle.ny
	@cp $(BUILD_DIR)/libnytrixrt.so $(DESTDIR)$(LIBDIR)/nytrix/libnytrixrt.so
	@chmod 755 $(DESTDIR)$(LIBDIR)/nytrix/libnytrixrt.so
	@echo "  $(C_GREEN)✓ Installed$(C_RESET)"

uninstall:
	@echo "  $(C_GRAY)UNINSTALL$(C_RESET) Removing $(BIN_NAME) from $(DESTDIR)$(BINDIR)"
	@rm -f $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@rm -f $(DESTDIR)$(SHAREDIR)/nytrix/std_bundle.ny
	@rm -f $(DESTDIR)$(LIBDIR)/nytrix/libnytrixrt.so
	@rmdir $(DESTDIR)$(SHAREDIR)/nytrix 2>/dev/null || true
	@rmdir $(DESTDIR)$(LIBDIR)/nytrix 2>/dev/null || true
	@echo "  $(C_GREEN)✓ Uninstalled$(C_RESET)"

test: $(BIN) $(BIN_DEBUG) $(BUILD_DIR)/std_bundle.ny
	@NYTRIX_STD_PREBUILT=$(BUILD_DIR)/std_bundle.ny python3 etc/tools/run_tests --bin $(BIN)

teststd: $(BIN_DEBUG) $(BUILD_DIR)/std_bundle.ny
	@python3 etc/tools/run_tests --bin $(BIN_DEBUG) --pattern "std"

testruntime: $(BIN_DEBUG) $(BUILD_DIR)/std_bundle.ny
	@python3 etc/tools/run_tests --bin $(BIN_DEBUG) --pattern "runtime"

testbench: $(BIN) $(BUILD_DIR)/std_bundle.ny
	@python3 etc/tools/run_tests --bin $(BIN) --pattern "bench"

bench: testbench

coverage:
	@$(MAKE) debug PROFFLAGS="-fprofile-instr-generate -fcoverage-mapping"

clean:
	@rm -rf $(BUILD_DIR) .tmp
	@find src etc -name "*.o" -delete
	@find src etc -name "*.so" -delete
	@find src etc -name "*.a" -delete
	@find . -name "*.tmp" -delete
	@find . -name "*.log" -delete
	@echo "  $(C_GRAY)CLEAN$(C_RESET) build artifacts removed"

tidy:
	@echo "  $(C_GRAY)TIDY$(C_RESET) Formatting code..."
	@find src etc -type f \( -name "*.c" -o -name "*.h" \) -not -path "*/.*" -exec clang-format -i {} +
	@find src etc -type f \( -name "*.ny" -o -name "*.py" -o -name "Makefile" -o -name "*.md" \) -not -path "*/.*" -exec sed -i -e 's/[[:space:]]*$$//' -e 's/\r//g' -e ':a; s/^\(\t*\)    /\1\t/; ta' {} +
	@echo "  $(C_GREEN)✓ Tidy complete$(C_RESET)"
ifneq ($(BEAR),)
	@mkdir -p ./etc/cache
	@$(BEAR) --output ./etc/cache/compile_commands.json -- $(MAKE) bin $(BIN_LSP) $(BUILD_DIR)/std_bundle.ny $(BUILD_DIR)/libnytrixrt.so > /dev/null 2>&1
else
endif
