# ---------------------------------------------------------------------------
# Top-level Makefile — thin PGXS-compatible wrapper around the CMake build.
#
# CMakeLists.txt remains the source of truth.  This file exists so that the
# standard PGXS workflow works out of the box:
#
#     make                # configure + build (Release)
#     sudo make install   # install to $(pg_config --pkglibdir)/share
#     make installcheck   # run regression suite against the installed extension
#     make clean
#     make uninstall
#
# In particular it lets PGXN's client install the extension via
#
#     pgxn install pg_orca
#
# which internally runs `make USE_PGXS=1` + `make USE_PGXS=1 install`.  We
# accept (and ignore) USE_PGXS=1 — we don't actually pull in pgxs.mk, since
# pgxs's single-target $(MODULES) model doesn't fit a 4-library C++ project
# with xerces-c linkage.  CMake handles all of that.
#
# Variables you may override on the command line:
#
#     PG_CONFIG=/path/to/pg_config    (default: first pg_config on PATH)
#     BUILD_TYPE=Release|Debug|...    (default: Release)
#     BUILD_DIR=build                 (default: build)
#     JOBS=N                          (default: $(nproc))
#     GENERATOR="Ninja"|"Unix Makefiles"   (default: Ninja if available)
#     DESTDIR=/tmp/stage              passed through to cmake --install
# ---------------------------------------------------------------------------

PG_CONFIG  ?= pg_config
BUILD_TYPE ?= Release
BUILD_DIR  ?= build
JOBS       ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Prefer Ninja; fall back to Unix Makefiles if it's not installed.
GENERATOR  ?= $(shell command -v ninja >/dev/null 2>&1 && echo Ninja || echo "Unix Makefiles")

CMAKE      ?= cmake
CMAKE_ARGS ?=
CONFIGURE_STAMP = $(BUILD_DIR)/CMakeCache.txt

.PHONY: all configure build install installcheck clean distclean uninstall help

all: build

# --- configure ------------------------------------------------------------
$(CONFIGURE_STAMP):
	@echo ">>> configuring CMake (BUILD_TYPE=$(BUILD_TYPE), generator='$(GENERATOR)')"
	$(CMAKE) -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
	    -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	    -DPG_CONFIG=$(PG_CONFIG) \
	    $(CMAKE_ARGS)

configure: $(CONFIGURE_STAMP)

# --- build ----------------------------------------------------------------
build: configure
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

# --- install --------------------------------------------------------------
# DESTDIR is the GNU-make convention for staged installs and is honored by
# cmake --install via the env var (package builders like rpm/deb rely on it).
# Empty DESTDIR is safe — cmake treats it as "install to absolute paths".
install: build
	DESTDIR=$(DESTDIR) $(CMAKE) --install $(BUILD_DIR)

# --- regression check -----------------------------------------------------
# Runs the pg_orca-specific regression schedule against an installed
# extension.  PG_CONFIG must point to the same instance the extension is
# installed in; PG_REGRESS_SQL must point to the PG source tree's
# src/test/regress (the schedules reference files there).
installcheck:
	@if [ -z "$$PG_REGRESS_SQL" ]; then \
	    echo "PG_REGRESS_SQL must be set to PostgreSQL's src/test/regress directory" >&2; \
	    echo "  e.g.: export PG_REGRESS_SQL=/path/to/postgresql/src/test/regress" >&2; \
	    exit 1; \
	fi
	PG_CONFIG=$(PG_CONFIG) test/test.sh --orca-tests

# --- clean ----------------------------------------------------------------
clean:
	@if [ -d $(BUILD_DIR) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf $(BUILD_DIR)

# --- uninstall ------------------------------------------------------------
# CMake does not generate an uninstall target by default, but it records every
# installed path in $(BUILD_DIR)/install_manifest.txt — remove those.
uninstall:
	@if [ ! -f $(BUILD_DIR)/install_manifest.txt ]; then \
	    echo "No $(BUILD_DIR)/install_manifest.txt — nothing to uninstall." >&2; \
	    exit 1; \
	fi
	xargs rm -fv < $(BUILD_DIR)/install_manifest.txt

# --- help -----------------------------------------------------------------
help:
	@echo "Targets:"
	@echo "  make              configure + build (Release)"
	@echo "  make install      install to PG's pkglibdir / sharedir"
	@echo "  make installcheck run ORCA regression tests (needs PG_REGRESS_SQL)"
	@echo "  make clean        clean build artifacts"
	@echo "  make distclean    remove the entire build directory"
	@echo "  make uninstall    remove installed files (uses install_manifest.txt)"
	@echo ""
	@echo "Variables (override on cmdline, e.g. make PG_CONFIG=/usr/pgsql-18/bin/pg_config):"
	@echo "  PG_CONFIG    path to pg_config              [$(PG_CONFIG)]"
	@echo "  BUILD_TYPE   CMake build type               [$(BUILD_TYPE)]"
	@echo "  BUILD_DIR    out-of-tree build directory    [$(BUILD_DIR)]"
	@echo "  JOBS         parallel build jobs            [$(JOBS)]"
	@echo "  GENERATOR    CMake generator                [$(GENERATOR)]"
	@echo "  CMAKE_ARGS   extra args passed to cmake     [$(CMAKE_ARGS)]"
