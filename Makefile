# ==============================================================================
# sqlsh - minimal SQLite shell with zsh-style completion
# Derived from ~/computation/programming/c/template, adapted for C99 + sqlite3.
# ==============================================================================

# `CC ?=` does not work here: make predefines CC, so ?= never fires.
# Only override when the value is make's own default.
ifeq ($(origin CC),default)
  CC = clang
endif
STD = -std=c99

TARGET_NAME = sqlsh
BIN_DIR   = bin
SRC_DIR   = src
INC_DIR   = include
TEST_DIR  = tests
PREFIX   ?= /usr/local

# ------------------------------------------------------------------------------
# Build mode
#
# Objects live in build/$(SQLITE)/$(MODE) so that switching between, say,
# debug and asan cannot silently reuse objects compiled with the other mode's
# flags. The template shares one build/ across modes, which makes `make test`
# link uninstrumented objects if a debug build ran first.
#
# The mode targets below re-invoke make with MODE set; build rules only ever
# see one mode at a time.
# ------------------------------------------------------------------------------
MODE ?= debug

ifeq ($(MODE),release)
  # No -march=native: this binary is portable, and the workload is I/O and
  # terminal bound, not arithmetic bound.
  MODE_CFLAGS  = -O2 -DNDEBUG
  MODE_LDFLAGS =
else ifeq ($(MODE),debug)
  MODE_CFLAGS  = -O0 -g3 -DDEBUG
  MODE_LDFLAGS =
else ifeq ($(MODE),asan)
  MODE_CFLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -g3 -O1 -DDEBUG
  MODE_LDFLAGS = -fsanitize=address,undefined
else ifeq ($(MODE),msan)
  MODE_CFLAGS  = -fsanitize=memory -fsanitize-memory-track-origins \
                 -fno-omit-frame-pointer -g3 -O1 -DDEBUG
  MODE_LDFLAGS = -fsanitize=memory
else
  $(error unknown MODE '$(MODE)': expected release, debug, asan or msan)
endif

# ------------------------------------------------------------------------------
# SQLite backend: system library (default) or vendored amalgamation
#   make                   -> pkg-config sqlite3
#   make SQLITE=vendored   -> compile sqlite3.c from $(SQLITE_AMALGAMATION)
# SQLITE_AMALGAMATION is exported by the nix devShell.
# ------------------------------------------------------------------------------
SQLITE ?= system

# Objects are keyed by backend as well as mode: the two backends compile with
# different include paths, so they must not share a directory either.
BUILD_DIR = build/$(SQLITE)/$(MODE)

ifeq ($(SQLITE),vendored)
  ifeq ($(SQLITE_AMALGAMATION),)
    $(error SQLITE=vendored requires SQLITE_AMALGAMATION to point at a directory \
containing sqlite3.c and sqlite3.h. Enter the nix devShell, or set it by hand)
  endif
  SQLITE_CFLAGS = -I$(SQLITE_AMALGAMATION)
  SQLITE_LIBS   = -lpthread -ldl -lm
  SQLITE_OBJ    = $(BUILD_DIR)/vendor/sqlite3.o
  # Trim the amalgamation to what a shell actually needs.
  SQLITE_AMALGAMATION_CFLAGS = \
	-DSQLITE_ENABLE_COLUMN_METADATA=1 \
	-DSQLITE_ENABLE_FTS5=1 \
	-DSQLITE_ENABLE_JSON1=1 \
	-DSQLITE_OMIT_DEPRECATED=1 \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_DQS=0 \
	-O2
else
  SQLITE_CFLAGS = $(shell pkg-config --cflags sqlite3)
  SQLITE_LIBS   = $(shell pkg-config --libs sqlite3)
  SQLITE_OBJ    =
endif

# Strict warning flags (inherited from the template)
WARNING_FLAGS = \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Wshadow \
	-Wconversion \
	-Wsign-conversion \
	-Wnull-dereference \
	-Wdouble-promotion \
	-Wformat=2 \
	-Wformat-security \
	-Wundef \
	-Wstrict-prototypes \
	-Wmissing-prototypes \
	-Wredundant-decls \
	-Wmissing-declarations \
	-Wcast-align \
	-Wcast-qual \
	-Wwrite-strings

# The gate sets this so warnings fail the build. It is off by default: during
# development a warning should be visible without stopping the compile.
ifeq ($(WARNINGS_AS_ERRORS),1)
  WARNING_FLAGS += -Werror
endif

# X/Open 7 under -std=c99, which subsumes POSIX.1-2008: termios, isatty and
# sigaction, plus posix_openpt/grantpt/ptsname for the pty test harness.
# Defined here rather than in the sources so no translation unit has to declare
# a reserved identifier of its own.
FEATURE_FLAGS = -D_XOPEN_SOURCE=700

# -Isrc so tests can reach internal headers without a separate compile of the
# same translation units.
INCLUDES = -I$(INC_DIR) -I$(SRC_DIR) $(SQLITE_CFLAGS)

CFLAGS  = $(STD) $(FEATURE_FLAGS) $(WARNING_FLAGS) $(INCLUDES) $(MODE_CFLAGS)
LDFLAGS = $(MODE_LDFLAGS)

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
# fuzz_tokenizer.c is excluded here: it is LLVMFuzzerTestOneInput, not a
# minunit suite, and it has its own build rule below under -fsanitize=fuzzer.
# Left in this glob it would compile into test_runner too, where nothing
# calls it and -Wmissing-prototypes fires on the libFuzzer entry point.
TEST_SRCS = $(filter-out $(TEST_DIR)/fuzz_tokenizer.c, $(wildcard $(TEST_DIR)/*.c))
TEST_OBJS = $(patsubst $(TEST_DIR)/%.c, $(BUILD_DIR)/tests/%.o, $(TEST_SRCS))

.PHONY: all release debug asan msan binary test run-tests fixtures reference \
        valgrind tidy cppcheck format format-check parity gate compdb watch \
        fuzz install clean help

all: debug

# ------------------------------------------------------------------------------
# Mode entry points: each re-invokes make with MODE pinned
# ------------------------------------------------------------------------------

release debug asan msan:
	@$(MAKE) --no-print-directory MODE=$@ binary

binary: $(BIN_DIR)/$(TARGET_NAME)

# The binary is built per mode and copied to bin/, so bin/sqlsh always reflects
# the mode that was built last rather than whichever object happened to be new.
$(BIN_DIR)/$(TARGET_NAME): $(BUILD_DIR)/$(TARGET_NAME) | $(BIN_DIR)
	cp -f $< $@

$(BUILD_DIR)/$(TARGET_NAME): $(OBJS) $(SQLITE_OBJ)
	$(CC) $(OBJS) $(SQLITE_OBJ) -o $@ $(LDFLAGS) $(SQLITE_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The amalgamation is third-party: compile it without our warning flags and
# without sanitizers, which would only slow it down and report nothing useful.
$(BUILD_DIR)/vendor/sqlite3.o: $(SQLITE_AMALGAMATION)/sqlite3.c | $(BUILD_DIR)/vendor
	$(CC) $(STD) $(SQLITE_AMALGAMATION_CFLAGS) -w -c $< -o $@

-include $(OBJS:.o=.d)
-include $(TEST_OBJS:.o=.d)

# ------------------------------------------------------------------------------
# Tests: always under ASan+UBSan, in their own object tree
# ------------------------------------------------------------------------------

test:
	@$(MAKE) --no-print-directory MODE=asan run-tests

# The completion tests read tests/test.db, so it is a prerequisite rather than
# something the developer has to remember to build.
run-tests: $(BUILD_DIR)/tests/test_runner $(TEST_DIR)/test.db
	@echo "Running test suite ($(MODE))..."
	@ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" ./$(BUILD_DIR)/tests/test_runner

$(BUILD_DIR)/tests/test_runner: $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) \
                                $(TEST_OBJS) $(SQLITE_OBJ) | $(BUILD_DIR)/tests
	$(CC) $(filter %.o, $^) -o $@ $(LDFLAGS) $(SQLITE_LIBS)

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Regenerate the test fixture database from its SQL source.
fixtures: $(TEST_DIR)/test.db

$(TEST_DIR)/test.db: $(TEST_DIR)/fixtures.sql
	@rm -f $@
	sqlite3 $@ < $<
	@echo "Built $@"

# ------------------------------------------------------------------------------
# Reference material
# ------------------------------------------------------------------------------

# sqlite's own shell.c, for consulting behaviour we intend to match.
# Not vendored into git: 34k generated lines (see docs/ARCHITECTURE.md).
# Taken from the amalgamation the flake already fetched, so there is one
# pinned sqlite version in the project rather than two.
reference: reference/shell.c

reference/shell.c:
ifeq ($(SQLITE_AMALGAMATION),)
	@echo "make reference needs SQLITE_AMALGAMATION; enter the nix devShell." >&2
	@false
else
	@mkdir -p reference
	cp -f $(SQLITE_AMALGAMATION)/shell.c $@
	@chmod u+w $@
	@echo "Copied upstream shell.c into reference/ for lookup only."
endif

# ------------------------------------------------------------------------------
# Quality & static analysis
# ------------------------------------------------------------------------------

valgrind: debug fixtures $(BUILD_DIR)/tests/test_runner
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
	         --error-exitcode=1 ./$(BIN_DIR)/$(TARGET_NAME) $(TEST_DIR)/test.db \
	         'SELECT * FROM employees LIMIT 3;'
	# The unit suite runs uninstrumented here rather than under `make test`'s
	# ASan+UBSan build: both ASan and valgrind intercept malloc, and running
	# one under the other reports nothing useful. MODE defaults to debug for
	# this target (matching the `debug` prerequisite above), which is what
	# builds $(BUILD_DIR)/tests/test_runner the plain way. --trace-children
	# is what lets valgrind follow test_sigterm_restores_termios's fork(), so
	# this is what makes the pty suite (line_suite) actually covered, not
	# just skipped as "already sanitized elsewhere".
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
	         --trace-children=yes --error-exitcode=1 \
	         ./$(BUILD_DIR)/tests/test_runner

# clang-tidy runs off compile_commands.json rather than a hand-passed flag
# list, because flags after `--` lose the toolchain's own include paths.
#
# That is not sufficient on its own under nix: the cc-wrapper injects glibc's
# include directory, and clang-tidy invokes the unwrapped clang, so <stdio.h>
# goes missing, FILE degrades to int, and a cascade of bogus warnings buries
# the real ones. Ask the compiler where its headers actually are.
TIDY_SYS_INCLUDES = $(shell $(CC) -E -Wp,-v -xc /dev/null 2>&1 | \
	sed -n 's|^ \(/[^ ]*\)$$|--extra-arg-before=-isystem\1|p')

# Belt and braces for toolchains that inject -D_FORTIFY_SOURCE regardless of
# optimisation level: at the -O0 of the compdb, glibc turns that into a
# #warning in every file. The devShell disables the hardening flag; this
# undoes it for anyone building outside the flake. Appended, not prepended,
# so it wins over a -D that the compdb already carries.
TIDY_EXTRA = --extra-arg=-U_FORTIFY_SOURCE

tidy: compile_commands.json
	clang-tidy -p . $(TIDY_SYS_INCLUDES) $(TIDY_EXTRA) $(SRCS) $(TEST_SRCS) $(TEST_DIR)/fuzz_tokenizer.c

# A libFuzzer target over sqlctx.c's lexer and cursor-context analysis (see
# tests/fuzz_tokenizer.c for what it exercises and why that module). Its own
# build, not a MODE: libFuzzer needs -fsanitize=fuzzer, which nothing else in
# this Makefile asks for, and the target is sqlctx.c alone -- it is the one
# module that touches neither the heap nor a database, so nothing else needs
# to be linked in.
#
# FUZZ_TIME bounds the run so it fits a CI job instead of running forever;
# override it (make fuzz FUZZ_TIME=300) for a longer local session. A crash
# or a hang past -timeout writes its reproducer next to the binary.
FUZZ_TIME ?= 30
FUZZ_BIN = build/fuzz_tokenizer

fuzz: $(FUZZ_BIN)
	./$(FUZZ_BIN) -max_total_time=$(FUZZ_TIME) -timeout=5

$(FUZZ_BIN): $(TEST_DIR)/fuzz_tokenizer.c $(SRC_DIR)/sqlctx.c $(INC_DIR)/sqlctx.h | build
	$(CC) $(STD) $(FEATURE_FLAGS) $(WARNING_FLAGS) -I$(INC_DIR) -I$(SRC_DIR) \
	      -fsanitize=fuzzer,address,undefined -g -O1 \
	      $(TEST_DIR)/fuzz_tokenizer.c $(SRC_DIR)/sqlctx.c -o $@

build:
	mkdir -p $@

# --error-exitcode=1 makes any finding fail the gate, which means the purely
# informational notes have to be silenced individually rather than by dropping
# the exit code. Each is a statement about cppcheck's own analysis, not about
# this code:
#   checkersReport            - which checkers ran.
#   toomanyconfigs            - the #ifdef space of sqlite3.h, not of src/.
#   unusedFunction            - false on a two-binary build: the test runner
#                               and sqlsh each use a different part of db.c.
#   missingIncludeSystem      - system headers are the compiler's business.
# --check-level=exhaustive removes normalCheckLevelMaxBranches by actually
# analysing every branch; affordable on a codebase this size.
cppcheck:
	cppcheck --enable=all --check-level=exhaustive --inconclusive \
	         --error-exitcode=1 --std=c99 --inline-suppr \
	         --suppress=missingIncludeSystem --suppress=unusedFunction \
	         --suppress=checkersReport --suppress=toomanyconfigs \
	         $(INCLUDES) $(SRC_DIR) $(INC_DIR) $(TEST_DIR)

# Non-mutating counterpart of `format`, for the gate: fails if any tracked
# source would be changed by the formatter.
format-check:
	@clang-format --dry-run --Werror $(wildcard $(SRC_DIR)/*.c) \
	              $(wildcard $(INC_DIR)/*.h) $(wildcard $(TEST_DIR)/*.c) \
	              $(wildcard $(TEST_DIR)/*.h)

# Differential output-parity suite against sqlite3(1) (see tests/parity.sh).
# Skips cleanly when sqlite3 is absent; SKIP_PARITY=1 skips it unconditionally
# for whoever needs to run the gate without it.
parity: $(BIN_DIR)/$(TARGET_NAME)
	sh $(TEST_DIR)/parity.sh $(BIN_DIR)/$(TARGET_NAME)

# One command, one verdict. This is the phase gate: a phase is not done until
# `make gate` prints PASS. Every step below fails the build on any finding, so
# a warning is a defect rather than something to read past.
#
# Both SQLite backends are built because they are separate compilations and a
# warning can appear in only one of them.
gate:
	@echo "== gate: format ==";    $(MAKE) --no-print-directory format-check
	@echo "== gate: build (system) ==";   $(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory release WARNINGS_AS_ERRORS=1
	@echo "== gate: build (vendored) =="; $(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory SQLITE=vendored release WARNINGS_AS_ERRORS=1
	@echo "== gate: tests (asan+ubsan) =="; $(MAKE) --no-print-directory test
ifneq ($(SKIP_PARITY),1)
	@echo "== gate: parity ==";    $(MAKE) --no-print-directory parity
endif
	@echo "== gate: cppcheck ==";  $(MAKE) --no-print-directory cppcheck
	@echo "== gate: clang-tidy =="; $(MAKE) --no-print-directory tidy
	@echo "PASS"

format:
	clang-format -i $(wildcard $(SRC_DIR)/*.c) $(wildcard $(INC_DIR)/*.h) \
	                $(wildcard $(TEST_DIR)/*.c) $(wildcard $(TEST_DIR)/*.h)

compdb:
	@rm -f compile_commands.json
	@$(MAKE) --no-print-directory compile_commands.json

compile_commands.json:
	@$(MAKE) --no-print-directory clean
	bear -- $(MAKE) --no-print-directory debug

watch:
	watchexec -e c,h -c -- $(MAKE) test

# ------------------------------------------------------------------------------
# Housekeeping
# ------------------------------------------------------------------------------

install: release
	install -Dm755 $(BIN_DIR)/$(TARGET_NAME) $(DESTDIR)$(PREFIX)/bin/$(TARGET_NAME)
	install -Dm644 docs/sqlsh.1 $(DESTDIR)$(PREFIX)/share/man/man1/sqlsh.1

$(BIN_DIR) $(BUILD_DIR) $(BUILD_DIR)/tests $(BUILD_DIR)/vendor:
	mkdir -p $@

clean:
	rm -rf build $(BIN_DIR) compile_commands.json

help:
	@echo "sqlsh"
	@echo "  make / make debug     debug build (default)"
	@echo "  make release          optimised build"
	@echo "  make asan             AddressSanitizer + UBSan"
	@echo "  make msan             MemorySanitizer (clang)"
	@echo "  make test             unit tests under ASan"
	@echo "  make fixtures         regenerate tests/test.db"
	@echo "  make reference        fetch sqlite shell.c into reference/"
	@echo "  make SQLITE=vendored  build against the sqlite amalgamation"
	@echo "  make parity           differential output-parity suite vs sqlite3(1)"
	@echo "  make tidy cppcheck format compdb valgrind watch fuzz"
	@echo "  make install PREFIX=~/.local"
	@echo "  make gate SKIP_PARITY=1   skip the parity suite in the gate"
	@echo ""
	@echo "Objects live in build/<backend>/<mode>/ so builds never share stale objects."
