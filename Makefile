CC ?= cc
AR ?= ar

BUILDDIR ?= out
MODE ?= debug
SANITIZE ?= none
GUEST_PROFILE ?= debug
TEST_TIMEOUT ?= 300

PREFIX ?= /usr/local

PKG := libcbor libzstd wasmtime libcurl openssl libngtcp2 libngtcp2_crypto_ossl sqlite3
PKG_CFLAGS := $(shell pkg-config --cflags $(PKG))
PKG_LIBS := $(shell pkg-config --libs $(PKG))

WASI_CC := $(shell pkg-config --variable=clang wasi-sdk)

ifeq ($(MODE),release)
OPT := -O2
else
OPT := -g
endif

SAN_FLAGS :=
ifneq ($(SANITIZE),none)
SAN_FLAGS := -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif

CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror $(OPT) \
          -MMD -MP -Isrc $(PKG_CFLAGS) $(SAN_FLAGS)
LDFLAGS := $(SAN_FLAGS)

WASI_OPT := $(if $(filter release,$(GUEST_PROFILE)),-O2,-O0 -g)
WASI_FLAGS := --target=wasm32-wasip1 -nostdlib -Wl,--no-entry \
              -Wl,--export-memory $(WASI_OPT)

CORE_SRC := \
  src/proto/frame.c \
  src/proto/codec.c \
  src/proto/sse.c \
  src/core/util.c \
  src/core/ids.c \
  src/core/log.c \
  src/core/session.c \
  src/core/approval.c \
  src/core/turn.c \
  src/core/models.c \
  src/core/worker.c \
  src/core/labels.c \
  src/core/schedule.c \
  src/core/proc.c \
           src/core/pty.c \
           src/core/judge.c \
           src/core/manifest.c \
           src/core/registry.c \
           src/core/modstate.c \
           src/core/index.c \
           src/core/cgroup.c \
           src/core/credentials.c \
           src/core/devlock.c \
           src/core/files.c \
           src/core/presets.c \
           src/core/prompt.c \
           src/core/probe.c \
  src/wasm/host.c \
  src/net/http.c \
  src/net/pki.c \
  src/net/quic.c

CORE_OBJ := $(patsubst %.c,$(BUILDDIR)/%.o,$(CORE_SRC))
CLIENT_OBJ := $(BUILDDIR)/src/client/client.o
NATIVE := $(BUILDDIR)/ward-native
DESKTOP_BUILD ?= clients/desktop/build
DESKTOP_PROBE := $(DESKTOP_BUILD)/ward-gui-probe
DESKTOP_GUI := $(DESKTOP_BUILD)/ward-gui
BIN_OBJ := $(BUILDDIR)/src/cli/main.o $(BUILDDIR)/src/daemon/server.o
NATIVE_OBJ := $(BUILDDIR)/clients/native/main.o
CORE_LIB := $(BUILDDIR)/libward-core.a

WARD := $(BUILDDIR)/ward

TEST_BINS := $(patsubst tests/%.c,$(BUILDDIR)/%,$(wildcard tests/*.c))

MODULES := engine driver provider tools policy schedule skills judge code
MODULE_WASM := $(addprefix $(BUILDDIR)/,$(addsuffix .wasm,$(MODULES)))

GUESTS := echo wasi importer trap
GUEST_WASM := $(addprefix $(BUILDDIR)/,$(addsuffix .wasm,$(GUESTS)))

.PHONY: all test guests modules clean install check-deps desktop FORCE

all: check-deps $(WARD) $(NATIVE) $(MODULE_WASM) $(GUEST_WASM) $(TEST_BINS)

check-deps:
	@pkg-config --exists --print-errors 'libcbor >= 0.10'
	@pkg-config --exists --print-errors 'libzstd >= 1.4'
	@pkg-config --exists --print-errors 'libngtcp2 >= 1.0'
	@test -n "$(WASI_CC)" || { echo "wasi-sdk not found via pkg-config" >&2; exit 1; }

$(CORE_LIB): $(CORE_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(WARD): $(BIN_OBJ) $(CORE_LIB)
	$(CC) $(CFLAGS) $(BIN_OBJ) $(CLIENT_OBJ) $(CORE_LIB) $(LDFLAGS) $(PKG_LIBS) -o $@

$(NATIVE): $(NATIVE_OBJ) $(CLIENT_OBJ) $(CORE_LIB)
	$(CC) $(CFLAGS) $(NATIVE_OBJ) $(CLIENT_OBJ) $(CORE_LIB) $(LDFLAGS) $(PKG_LIBS) -o $@

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%: tests/%.c $(CLIENT_OBJ) $(CORE_LIB)
	$(CC) $(CFLAGS) $< $(CLIENT_OBJ) $(CORE_LIB) $(LDFLAGS) $(PKG_LIBS) -o $@

JAVA_HOME ?= $(shell dirname $$(dirname $$(readlink -f $$(which javac 2>/dev/null) 2>/dev/null) 2>/dev/null) 2>/dev/null)
ANDROID_LIB := $(BUILDDIR)/libwardclient.so

android-lib: $(ANDROID_LIB)

$(ANDROID_LIB): clients/android-ward/ward_jni.c src/client/client.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -fPIC -shared \
	  -Isrc -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux \
	  $(PKG_CFLAGS) clients/android-ward/ward_jni.c src/client/client.c \
	  src/proto/codec.c src/proto/frame.c src/core/util.c \
	  $(LDFLAGS) $(PKG_LIBS) -o $@

android-test: $(ANDROID_LIB)
	@mkdir -p $(BUILDDIR)/java
	$(JAVA_HOME)/bin/javac -d $(BUILDDIR)/java \
	  clients/android-ward/uk/xa0/ward/WardClient.java
	$(JAVA_HOME)/bin/java -Djava.library.path=$(abspath $(BUILDDIR)) \
	  -cp $(BUILDDIR)/java uk.xa0.ward.WardClient

desktop: $(DESKTOP_PROBE)

$(DESKTOP_PROBE): FORCE
	cmake -S clients/desktop -B $(DESKTOP_BUILD) -G Ninja
	cmake --build $(DESKTOP_BUILD)

$(DESKTOP_GUI): $(DESKTOP_PROBE)

FORCE:

modules: $(MODULE_WASM)
guests: $(GUEST_WASM)

$(BUILDDIR)/engine.wasm: modules/engine/engine.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/driver.wasm: modules/driver/driver.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/provider.wasm: modules/provider/provider.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/tools.wasm: modules/tools/tools.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/skills.wasm: modules/skills/skills.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/schedule.wasm: modules/schedule/schedule.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/policy.wasm: modules/policy/policy.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/judge.wasm: modules/judge/judge.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/code.wasm: modules/code/code.c modules/abi/wd_abi.h
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) -I modules/abi $< -o $@

$(BUILDDIR)/echo.wasm: tests/guest/echo.c
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) $< -o $@

$(BUILDDIR)/wasi.wasm: tests/guest/wasi.c
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) $< -o $@

$(BUILDDIR)/trap.wasm: tests/guest/trap.c
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) $< -o $@

$(BUILDDIR)/importer.wasm: tests/guest/importer.c
	@mkdir -p $(@D)
	$(WASI_CC) $(WASI_FLAGS) $< -o $@

test: $(TEST_BINS) $(WARD) $(NATIVE) $(DESKTOP_PROBE) $(MODULE_WASM) $(GUEST_WASM)
	@set -u; \
	fail=0; \
	for t in $(TEST_BINS); do \
	  name=$$(basename $$t); \
	  printf '%-14s ' "$$name"; \
	  if WARD_BIN=$(abspath $(WARD)) \
	     WARD_NATIVE=$(abspath $(NATIVE)) \
	     WARD_GUI_PROBE=$(abspath $(DESKTOP_PROBE)) \
	     WARD_GUI=$(abspath $(DESKTOP_GUI)) \
	     WARD_MODULE_DIR=$(abspath $(BUILDDIR)) \
	     WARD_GUEST_DIR=$(abspath $(BUILDDIR)) \
	     timeout $(TEST_TIMEOUT) $$t >$(BUILDDIR)/$$name.log 2>&1; then \
	    echo ok; \
	  else \
	    echo FAILED; fail=1; tail -n 20 $(BUILDDIR)/$$name.log; \
	  fi; \
	done; \
	exit $$fail

install: $(WARD)
	install -Dm755 $(WARD) $(DESTDIR)$(PREFIX)/bin/ward

clean:
	rm -rf -- $(BUILDDIR)

-include $(CORE_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) $(BIN_OBJ:.o=.d) $(NATIVE_OBJ:.o=.d) $(TEST_BINS:=.d)

.DELETE_ON_ERROR:
