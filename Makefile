.DEFAULT_GOAL := all

CC ?= clang
OPT ?= 3
SAN ?= 0
PROFILE ?= 0
PREFIX ?= /usr
DESTDIR ?=
BUILD_DIR ?= build
LOG_DIR   ?= $(BUILD_DIR)/logs
LLVM_CONFIG ?= llvm-config

JOBS      := $(shell expr $$(nproc) / 2)
MAKEFLAGS += -j$(JOBS) --no-print-directory

# Protect build tools from user environment leaks
unexport LD_PRELOAD
export ASAN_OPTIONS=detect_leaks=0

BIN_NAME := ny
BIN       := $(BUILD_DIR)/$(BIN_NAME)
BIN_DEBUG := $(BUILD_DIR)/$(BIN_NAME)_debug
BIN_LSP   := $(BUILD_DIR)/ny-lsp
STD_BUNDLE := $(BUILD_DIR)/std.ny
FUZZ_ITERS ?= 200
FUZZ_TIMEOUT ?= 1.2
FUZZ_JOBS ?= $(JOBS)
FUZZ_MODE ?= mixed
FUZZ_FLAGS ?=

TIDY_DIRS := src std etc/examples

C_RESET  := $(shell printf '\033[0m')
C_GRAY   := $(shell printf '\033[90m')
C_GREEN  := $(shell printf '\033[32m')
C_CYAN   := $(shell printf '\033[1;36m')

LLVM_CFLAGS  := $(shell $(LLVM_CONFIG) --cflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core native mcjit)

# Check if bear is available
BEAR := $(shell command -v bear 2>/dev/null)

SANFLAGS :=
ASAN_LDFLAGS :=
LD_PRELOAD_ASAN :=
ifeq ($(SAN),1)
SANFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
ASAN_LDFLAGS := -fsanitize=address,undefined
LD_PRELOAD_ASAN := /usr/lib/clang/21/lib/linux/libclang_rt.asan-x86_64.so
endif
PROFFLAGS :=
ifeq ($(PROFILE),1)
PROFFLAGS += -pg
endif

OPTFLAGS := -O$(OPT)

CFLAGS_BASE := -std=c11 -g -fno-omit-frame-pointer -Wall -Wextra -Wshadow -Wstrict-prototypes -Wundef -Wcast-align -Wwrite-strings -Wunused -Isrc -Isrc/base -Isrc/rt -I$(BUILD_DIR) -I/usr/include -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -march=x86-64 -DNYTRIX_STD_PATH="\"$(PREFIX)/share/nytrix/std.ny\"" -DVERBOSE_BUILD
CFLAGS_DEBUG   := $(CFLAGS_BASE) -O0 -DDEBUG $(SANFLAGS) $(PROFFLAGS)
CFLAGS_RELEASE := $(CFLAGS_BASE) $(OPTFLAGS) -DNDEBUG $(SANFLAGS) $(PROFFLAGS)

LDFLAGS := $(ASAN_LDFLAGS) $(LLVM_LDFLAGS) -lreadline -lm -lc -rdynamic $(PROFFLAGS)

# Compiler Sources (Lib)
# src subdirs
SRC_COMPILER_DIRS := src/ast src/base src/lex src/sema src/code src/repl src/wire
SRC_COMPILER := $(foreach dir,$(SRC_COMPILER_DIRS),$(wildcard $(dir)/*.c))
SRC_COMPILER += src/parse/shared.c
SRC_RUNTIME := src/rt/init.c
RT_AMALGAM_PARTS := $(filter-out src/rt/init.c,$(wildcard src/rt/*.c))

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

.PHONY: all bin debug repl lsp ny-lsp help clean test fuzz tidy build install uninstall coverage install-man uninstall-man docs

all: bin lsp $(STD_BUNDLE) $(BUILD_DIR)/libnytrixrt.so

bin: $(BIN)

debug: $(BIN_DEBUG)

lsp: $(BIN_LSP)

ny-lsp: lsp

docs: $(BUILD_DIR)/nytrix.info $(BUILD_DIR)/ny.info $(BUILD_DIR)/nytrix.1 $(BUILD_DIR)/ny.1 $(STD_BUNDLE) | build
	@echo "  $(C_CYAN)WEBDOC$(C_RESET) generating documentation at $(BUILD_DIR)/docs/index.html..."
	@mkdir -p /tmp/nytrix-info
	@printf "ny mono file:///tmp/nytrix-info/NY.html\nny node file:///tmp/nytrix-info/\nnytrix mono file:///tmp/nytrix-info/NYTRIX.html\nnytrix node file:///tmp/nytrix-info/\n" > /tmp/nytrix-info/htmlxref.cnf
	@makeinfo --no-split --html --conf-dir=/tmp/nytrix-info etc/assets/info/ny.texi -o /tmp/nytrix-info/NY.html
	@makeinfo --no-split --html --conf-dir=/tmp/nytrix-info etc/assets/info/nytrix.texi -o /tmp/nytrix-info/NYTRIX.html
	@python3 etc/tools/conv etc/assets/info/ny.texi NY --format=md > /tmp/nytrix-info/NY.md
	@python3 etc/tools/conv etc/assets/info/nytrix.texi NYTRIX --format=md > /tmp/nytrix-info/NYTRIX.md
	@python3 etc/tools/webdoc $(STD_BUNDLE) -o $(BUILD_DIR)/docs

repl: $(BIN) $(STD_BUNDLE)
	@$(BIN) -i

help:
	@echo "\n$(C_CYAN)Nytrix Build System$(C_RESET)"
	@echo "$(C_GRAY)--------------------------------------------------$(C_RESET)"
	@echo "$(C_GREEN)make$(C_RESET)                      Build release + std bundle + runtime so"
	@echo "$(C_GREEN)make bin$(C_RESET)                  Build release executable ($(BIN))"
	@echo "$(C_GREEN)make debug$(C_RESET)                Build debug executable ($(BIN_DEBUG))"
	@echo "$(C_GREEN)make repl$(C_RESET)                 Run REPL (release)"
	@echo "$(C_GREEN)make test$(C_RESET)                 Run performance + unit tests (15s timeout)"
	@echo "$(C_GREEN)make fuzz$(C_RESET)                 Run parallel fuzz harness (etc/tools/fuzz)"
	@echo "$(C_GREEN)make install$(C_RESET)              Install to $(DESTDIR)$(BINDIR)/$(BIN_NAME)"
	@echo "$(C_GREEN)make clean$(C_RESET)                Remove build artifacts"
	@echo "$(C_GREEN)make tidy$(C_RESET)                 Format code using clang-format"
	@echo "$(C_GREEN)make docs$(C_RESET)                 Generate documentation"
	@echo ""
	@echo "$(C_CYAN)Build flags:$(C_RESET)"
	@echo "  SAN=1        Enable AddressSanitizer"
	@echo "  PROFILE=1    Enable profiling"
	@echo "  OPT=N        Optimization level (0-3)"
	@echo ""

build:
	@mkdir -p \
		$(BUILD_DIR)/compiler/debug/ast $(BUILD_DIR)/compiler/debug/code $(BUILD_DIR)/compiler/debug/lex $(BUILD_DIR)/compiler/debug/base $(BUILD_DIR)/compiler/debug/repl $(BUILD_DIR)/compiler/debug/wire $(BUILD_DIR)/compiler/debug/parse \
		$(BUILD_DIR)/compiler/release/ast $(BUILD_DIR)/compiler/release/code $(BUILD_DIR)/compiler/release/lex $(BUILD_DIR)/compiler/release/base $(BUILD_DIR)/compiler/release/repl $(BUILD_DIR)/compiler/release/wire $(BUILD_DIR)/compiler/release/parse \
		$(BUILD_DIR)/rt/debug $(BUILD_DIR)/rt/release $(BUILD_DIR)/rt/shared \
		$(BUILD_DIR)/cmd/ny $(BUILD_DIR)/cmd/ny-lsp
	@chmod -R a+rw $(BUILD_DIR) 2>/dev/null || true

$(STD_BUNDLE): $(wildcard std/**/*.ny) etc/tools/stdbundle | build
	@python3 etc/tools/stdbundle $@

$(BUILD_DIR)/std_symbols.h: $(STD_BUNDLE) | build
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

# Runtime is built from src/rt/init.c, which textual-includes the other
# src/rt/*.c files. Track those includes so edits rebuild init.o reliably.
$(BUILD_DIR)/rt/debug/init.o: $(RT_AMALGAM_PARTS)

$(BUILD_DIR)/rt/release/%.o: src/rt/%.c | build
	@echo "  $(C_GRAY)CC (release)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(BUILD_DIR)/rt/release/init.o: $(RT_AMALGAM_PARTS)

$(BUILD_DIR)/rt/shared/%.o: src/rt/%.c | build
	@echo "  $(C_GRAY)CC (shared)$(C_RESET) $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_RELEASE) -fPIC -c $< -o $@

$(BUILD_DIR)/rt/shared/init.o: $(RT_AMALGAM_PARTS)

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
	@$(CC) $(ASAN_LDFLAGS) -shared -Wl,-soname,libnytrixrt.so -o $@ $(OBJ_RUNTIME_SHARED) -ldl -lpthread $(PROFFLAGS)

$(BIN_DEBUG): $(OBJ_COMPILER_DEBUG) $(OBJ_RUNTIME_DEBUG) $(OBJ_CMD_NY_DEBUG) | build
	@echo "  $(C_GREEN)LD (debug)$(C_RESET) $@"
	@$(CC) $(ASAN_LDFLAGS) -o $@ $(OBJ_DEBUG) $(OBJ_CMD_NY_DEBUG) $(LDFLAGS)

$(BIN): $(OBJ_RELEASE) $(OBJ_CMD_NY_RELEASE) | build
	@echo "  $(C_GREEN)LD (release)$(C_RESET) $@"
	@$(CC) $(ASAN_LDFLAGS) -o $@ $(OBJ_RELEASE) $(OBJ_CMD_NY_RELEASE) $(LDFLAGS)

$(BIN_LSP): $(OBJ_CMD_LSP_RELEASE) $(OBJ_RELEASE) | build
	@echo "  $(C_GREEN)LD (lsp)$(C_RESET) $@"
	@$(CC) $(ASAN_LDFLAGS) -o $@ $(OBJ_CMD_LSP_RELEASE) $(OBJ_RELEASE) $(LDFLAGS)

BINDIR ?= /bin
LIBDIR ?= $(PREFIX)/lib
SHAREDIR ?= $(PREFIX)/share/nytrix
INFODIR  ?= $(PREFIX)/share/info
MANDIR   ?= $(PREFIX)/share/man

install: all install-info install-man
	@echo "  $(C_GRAY)INSTALL$(C_RESET) $(BIN_NAME) to $(DESTDIR)$(BINDIR)"
	@mkdir -p $(DESTDIR)$(BINDIR)
	@mkdir -p $(DESTDIR)$(SHAREDIR)
	@mkdir -p $(DESTDIR)$(LIBDIR)
	@cp $(BIN) $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@chmod 755 $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@cp $(BIN_LSP) $(DESTDIR)$(BINDIR)/ny-lsp
	@chmod 755 $(DESTDIR)$(BINDIR)/ny-lsp
	@cp $(STD_BUNDLE) $(DESTDIR)$(SHAREDIR)/std.ny
	@chmod 644 $(DESTDIR)$(SHAREDIR)/std.ny
	@cp $(BUILD_DIR)/libnytrixrt.so $(DESTDIR)$(LIBDIR)/libnytrixrt.so
	@chmod 755 $(DESTDIR)$(LIBDIR)/libnytrixrt.so
	@echo "  $(C_GRAY)INSTALL$(C_RESET) full source tree to $(DESTDIR)$(SHAREDIR)/src"
	@mkdir -p $(DESTDIR)$(SHAREDIR)/src
	@for dir in ast base cmd code lex parse repl rt wire; do \
		mkdir -p $(DESTDIR)$(SHAREDIR)/src/$$dir; \
		cp -r src/$$dir/* $(DESTDIR)$(SHAREDIR)/src/$$dir/ 2>/dev/null || true; \
	done
	@echo "  $(C_GRAY)INSTALL$(C_RESET) stdlib to $(DESTDIR)$(SHAREDIR)/std"
	@mkdir -p $(DESTDIR)$(SHAREDIR)/std
	@cp -r std/* $(DESTDIR)$(SHAREDIR)/std/
	@echo "  $(C_GREEN)✓ Installed$(C_RESET)"
	@if [ -n "$(SUDO_USER)" ] && [ "$(SUDO_USER)" != "root" ]; then \
		echo "  $(C_GRAY)RESTORE$(C_RESET) build folder ownership to $(SUDO_USER)"; \
		chown -R $(SUDO_USER):$(SUDO_USER) $(BUILD_DIR) 2>/dev/null || true; \
	fi

install-man: $(BUILD_DIR)/ny.1 $(BUILD_DIR)/nytrix.1
	@echo "  $(C_GRAY)INSTALL$(C_RESET) man pages to $(DESTDIR)$(MANDIR)/man1"
	@mkdir -p $(DESTDIR)$(MANDIR)/man1
	@cp $(BUILD_DIR)/ny.1 $(DESTDIR)$(MANDIR)/man1/ny.1
	@cp $(BUILD_DIR)/nytrix.1 $(DESTDIR)$(MANDIR)/man1/nytrix.1
	@chmod 644 $(DESTDIR)$(MANDIR)/man1/ny.1
	@chmod 644 $(DESTDIR)$(MANDIR)/man1/nytrix.1

install-info: $(BUILD_DIR)/nytrix.info $(BUILD_DIR)/ny.info
	@echo "  $(C_GRAY)INSTALL$(C_RESET) info pages to $(DESTDIR)$(INFODIR)"
	@mkdir -p $(DESTDIR)$(INFODIR)
	@cp $(BUILD_DIR)/nytrix.info $(DESTDIR)$(INFODIR)/nytrix.info
	@cp $(BUILD_DIR)/ny.info $(DESTDIR)$(INFODIR)/ny.info
	@if command -v install-info >/dev/null 2>&1; then \
		install-info --info-dir=$(DESTDIR)$(INFODIR) $(DESTDIR)$(INFODIR)/nytrix.info 2>/dev/null || true; \
		install-info --info-dir=$(DESTDIR)$(INFODIR) $(DESTDIR)$(INFODIR)/ny.info 2>/dev/null || true; \
	fi

$(BUILD_DIR)/%.info: etc/assets/info/%.texi | build
	@makeinfo $< -o $@

$(BUILD_DIR)/%.1: etc/assets/info/%.texi etc/tools/conv | build
	@python3 etc/tools/conv $< $* --format=man > $@

uninstall: uninstall-info uninstall-man
	@echo "  $(C_GRAY)UNINSTALL$(C_RESET) Removing $(BIN_NAME) and ny-lsp from $(DESTDIR)$(BINDIR)"
	@rm -f $(DESTDIR)$(BINDIR)/$(BIN_NAME)
	@rm -f $(DESTDIR)$(BINDIR)/ny-lsp
	@rm -rf $(DESTDIR)$(SHAREDIR)
	@rm -f $(DESTDIR)$(LIBDIR)/libnytrixrt.so
	@echo "  $(C_GREEN)✓ Uninstalled$(C_RESET)"

uninstall-man:
	@echo "  $(C_GRAY)UNINSTALL$(C_RESET) Removing man pages from $(DESTDIR)$(MANDIR)/man1"
	@rm -f $(DESTDIR)$(MANDIR)/man1/ny.1 $(DESTDIR)$(MANDIR)/man1/nytrix.1

uninstall-info:
	@echo "  $(C_GRAY)UNINSTALL$(C_RESET) Removing info pages from $(DESTDIR)$(INFODIR)"
	@if command -v install-info >/dev/null 2>&1; then \
		install-info --delete --info-dir=$(DESTDIR)$(INFODIR) $(DESTDIR)$(INFODIR)/nytrix.info 2>/dev/null || true; \
		install-info --delete --info-dir=$(DESTDIR)$(INFODIR) $(DESTDIR)$(INFODIR)/ny.info 2>/dev/null || true; \
	fi
	@rm -f $(DESTDIR)$(INFODIR)/nytrix.info $(DESTDIR)$(INFODIR)/ny.info

$(BUILD_DIR)/rt_debug.o: src/rt/init.c | build
	@echo "  $(C_GRAY)CC (debug-rt)$(C_RESET) $<"
	@$(CC) $(CFLAGS_DEBUG) -DNYTRIX_RUNTIME_ONLY -c $< -o $@

test: $(BIN_DEBUG) $(STD_BUNDLE)
	@mkdir -p $(LOG_DIR)
	@NYTRIX_BUILD_STD_PATH=$(STD_BUNDLE) NYTRIX_STD_PREBUILT=$(STD_BUNDLE) python3 etc/tools/tests --no-smoke --jobs $(JOBS)

fuzz: $(BIN_DEBUG) $(STD_BUNDLE)
	@mkdir -p $(LOG_DIR)
	@NYTRIX_STD_PREBUILT=$(STD_BUNDLE) ./etc/tools/fuzz --bin $(BIN_DEBUG) --iterations $(FUZZ_ITERS) --timeout $(FUZZ_TIMEOUT) -j $(FUZZ_JOBS) --mode $(FUZZ_MODE) $(FUZZ_FLAGS)

test-all: $(BIN_DEBUG) $(STD_BUNDLE)
	@pkill -9 -f "etc/tools/tests" || true
	@pkill -9 -f "build/ny" || true
	@mkdir -p $(LOG_DIR)
	@NYTRIX_ASAN=$(SAN) ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS="suppressions=$(CURDIR)/.lsan.supp" NYTRIX_STD_PREBUILT=$(STD_BUNDLE) python3 etc/tools/tests  --no-smoke --bin $(BIN_DEBUG) --log-dir $(LOG_DIR) --jobs $(JOBS)

ifeq ($(SAN),1)
	@echo ""
	@if [ -f $(LOG_DIR)/ASAN_SUMMARY.txt ]; then \
		echo "  $(C_CYAN)ASAN Summary Report:$(C_RESET)"; \
		cat $(LOG_DIR)/ASAN_SUMMARY.txt | head -20; \
		echo "  $(C_GRAY)Full report:$(C_RESET) $(LOG_DIR)/ASAN_SUMMARY.txt"; \
	fi
	@if ls $(LOG_DIR)/asan/*.* >/dev/null 2>&1; then \
		echo "  $(C_YELLOW)⚠ ASAN runtime logs found in:$(C_RESET) $(LOG_DIR)/asan/"; \
	fi
else ifeq ($(PROFILE),1)
	@echo "  $(C_CYAN)Generating gprof reports...$(C_RESET)"
	@mkdir -p $(LOG_DIR)/prof
	@for gmon in gmon.out etc/tests/**/gmon.out; do \
		if [ -f "$$gmon" ]; then \
			base=$$(basename $$(dirname $$gmon)); \
			gprof $(BIN_DEBUG) $$gmon > $(LOG_DIR)/prof/$$base.txt 2>/dev/null; \
			rm -f $$gmon; \
		fi; \
	done
	@if ls $(LOG_DIR)/prof/*.txt >/dev/null 2>&1; then \
		echo "  $(C_GREEN)✓ gprof reports saved to:$(C_RESET) $(LOG_DIR)/prof/"; \
		ls -1 $(LOG_DIR)/prof/ | head -5; \
	fi
endif

coverage:
	@export IS_CLANG=$$( $(CC) --version 2>&1 | grep -q clang && echo 1 || echo 0 ); \
	if [ "$$IS_CLANG" = "1" ]; then \
		$(MAKE) debug PROFFLAGS="-fprofile-instr-generate -fcoverage-mapping"; \
	else \
		$(MAKE) debug PROFFLAGS="--coverage"; \
	fi

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
	@python3 etc/tools/tidy $(TIDY_DIRS)
#	@echo "  $(C_GREEN)✓ Tidy complete$(C_RESET)"

bear:
ifneq ($(BEAR),)
	@echo "  $(C_GRAY)BEAR$(C_RESET) Generating compilation database..."
	@mkdir -p $(BUILD_DIR)/cache
	@$(BEAR) --output $(BUILD_DIR)/cache/compile_commands.json -- $(MAKE) bin $(BIN_LSP) $(STD_BUNDLE) $(BUILD_DIR)/libnytrixrt.so > /dev/null 2>&1
	@echo "  $(C_GREEN)✓ etc/cache/compile_commands.json updated$(C_RESET)"
else
	@echo "  $(C_RED)Error:$(C_RESET) bear not found in PATH"
	@exit 1
endif
