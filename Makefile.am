bin_PROGRAMS = fluxport-server fluxport-client
BIN_DIR = $(abs_builddir)/bin

AM_CPPFLAGS = \
	-D_POSIX_C_SOURCE=200809L \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/common \
	-I$(top_srcdir)/src/server \
	-I$(top_srcdir)/src/client

AM_CFLAGS = \
	-std=c11 \
	-Wall -Wextra -Wpedantic -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes

COMMON_SOURCES = \
	src/common/addr.c \
	src/common/bufq.c \
	src/common/config.c \
	src/common/frame.c \
	src/common/log.c \
	src/common/loop.c \
	src/common/net.c \
	src/common/protocol.c \
	src/common/tunnel_table.c \
	src/common/util.c

SERVER_SOURCES = \
	src/server/main.c \
	src/server/server.c \
	src/server/session.c \
	src/server/tunnel.c

CLIENT_SOURCES = \
	src/client/client.c \
	src/client/forwarder.c \
	src/client/main.c

fluxport_server_SOURCES = $(COMMON_SOURCES) $(SERVER_SOURCES)
fluxport_client_SOURCES = $(COMMON_SOURCES) $(CLIENT_SOURCES)

# Autotools links programs in the build root.  Keep that behavior (install
# rules depend on it) and publish a convenient copy under bin/.
all-local: $(bin_PROGRAMS)
	$(MKDIR_P) "$(BIN_DIR)"
	$(INSTALL_PROGRAM) "fluxport-server$(EXEEXT)" "$(BIN_DIR)/"
	$(INSTALL_PROGRAM) "fluxport-client$(EXEEXT)" "$(BIN_DIR)/"

clean-local:
	rm -rf "$(BIN_DIR)"

check_PROGRAMS = fp_testtool
fp_testtool_SOURCES = $(COMMON_SOURCES) tests/fp_testtool.c

TESTS = tests/run_tests.sh
TESTS_ENVIRONMENT = \
	FLUXPORT_SERVER="$(BIN_DIR)/fluxport-server$(EXEEXT)" \
	FLUXPORT_CLIENT="$(BIN_DIR)/fluxport-client$(EXEEXT)" \
	FLUXPORT_TESTTOOL="$(abs_builddir)/fp_testtool$(EXEEXT)" \
	FLUXPORT_TEST_WORK="$(abs_builddir)/tests-tmp"

# Convenience alias requested by the project specification.
test: all fp_testtool$(EXEEXT)
	@FLUXPORT_SERVER="$(BIN_DIR)/fluxport-server$(EXEEXT)" \
	 FLUXPORT_CLIENT="$(BIN_DIR)/fluxport-client$(EXEEXT)" \
	 FLUXPORT_TESTTOOL="$(abs_builddir)/fp_testtool$(EXEEXT)" \
	 FLUXPORT_TEST_WORK="$(abs_builddir)/tests-tmp" \
	 $(SHELL) "$(top_srcdir)/tests/run_tests.sh"

EXTRA_DIST = \
	README.md \
	docs/protocol.md \
	tests/run_tests.sh \
	tests/fp_testtool.c \
	do.txt \
	src/common/addr.h \
	src/common/bufq.h \
	src/common/config.h \
	src/common/frame.h \
	src/common/log.h \
	src/common/loop.h \
	src/common/net.h \
	src/common/protocol.h \
	src/common/tunnel_table.h \
	src/common/util.h \
	src/server/server.h \
	src/server/server_internal.h \
	src/client/client.h \
	src/client/client_internal.h
