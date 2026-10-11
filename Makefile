# Makefile for the Linux native JVM injector
#
#   make            Build everything
#   make injector   Build the ptrace injector
#   make payload    Build the payload .so
#   make unloader   Build the out-of-band unloader
#   make java       Compile the Java classes
#   make install    Copy everything to /tmp for testing
#   make clean      Remove build artifacts

JAVA_HOME ?= $(shell dirname $(shell dirname $(shell readlink -f $(shell which java))))
ifeq ($(JAVA_HOME),)
    JAVA_HOME = /usr/lib/jvm/java-8-openjdk-amd64
endif

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -ldl
JNI_INCLUDE = -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux

SRC_DIR = src
INJECT_DIR = $(SRC_DIR)/inject
PAYLOAD_DIR = $(SRC_DIR)/payload
JAVA_DIR = java
STUBS_DIR = stubs
BUILD_DIR = build
OUT_DIR = out
STUBS_OUT = $(BUILD_DIR)/stubs

INJECTOR = $(OUT_DIR)/injector
PAYLOAD = $(OUT_DIR)/payload.so
UNLOADER = $(OUT_DIR)/unloader

# The injector and unloader share the remote-process plumbing in remote.c.
REMOTE_SRC = $(INJECT_DIR)/remote.c $(INJECT_DIR)/remote.h
INJECTOR_SRC = $(INJECT_DIR)/injector.c $(INJECT_DIR)/remote.c
UNLOADER_SRC = $(INJECT_DIR)/unloader.c $(INJECT_DIR)/remote.c
PAYLOAD_SRC = $(wildcard $(PAYLOAD_DIR)/*.c)

# javac emits many .class files per stage, so gate each stage on a sentinel.
STUBS_SENTINEL = $(BUILD_DIR)/.stubs_built
MODULES_SENTINEL = $(BUILD_DIR)/.modules_built
HOOK_SENTINEL = $(BUILD_DIR)/.hook_built

CLIENT_SOURCES = $(wildcard $(JAVA_DIR)/client/*.java)
RENDERING_SOURCES = $(wildcard $(JAVA_DIR)/client/rendering/*.java)
ALL_MODULE_SOURCES = $(CLIENT_SOURCES) $(RENDERING_SOURCES)

.PHONY: all clean install injector payload unloader java help

all: $(INJECTOR) $(PAYLOAD) $(UNLOADER) $(HOOK_SENTINEL)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(STUBS_OUT):
	mkdir -p $(STUBS_OUT)

injector: $(INJECTOR)

$(INJECTOR): $(INJECTOR_SRC) $(INJECT_DIR)/remote.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -I$(INJECT_DIR) -o $@ $(INJECTOR_SRC) $(LDFLAGS)
	@echo "Built injector: $@"

payload: $(PAYLOAD)

$(PAYLOAD): $(PAYLOAD_SRC) $(PAYLOAD_DIR)/payload.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -shared -fPIC -I$(PAYLOAD_DIR) -o $@ $(PAYLOAD_SRC) $(JNI_INCLUDE) $(LDFLAGS) -lpthread
	@echo "Built payload: $@"

unloader: $(UNLOADER)

$(UNLOADER): $(UNLOADER_SRC) $(INJECT_DIR)/remote.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -I$(INJECT_DIR) -o $@ $(UNLOADER_SRC) $(LDFLAGS)
	@echo "Built unloader: $@"

# Java builds in three stages because of the package/stub dependency order:
#   1. MC class stubs (ave, avo) - compile-time only, never deployed
#   2. all client + rendering classes together (they cross-reference)
#   3. RenderHook, in the default package, needs the stubs and the client
java: $(HOOK_SENTINEL)

$(STUBS_SENTINEL): $(STUBS_DIR)/ave.java $(STUBS_DIR)/avo.java | $(STUBS_OUT) $(BUILD_DIR)
	javac -source 8 -target 8 -d $(STUBS_OUT) $(STUBS_DIR)/ave.java $(STUBS_DIR)/avo.java 2>/dev/null
	@touch $@
	@echo "Compiled MC stubs"

$(MODULES_SENTINEL): $(ALL_MODULE_SOURCES) | $(OUT_DIR) $(BUILD_DIR)
	javac -source 8 -target 8 -d $(OUT_DIR) $(ALL_MODULE_SOURCES) 2>/dev/null
	@touch $@
	@echo "Compiled client modules"

$(HOOK_SENTINEL): $(JAVA_DIR)/RenderHook.java $(STUBS_SENTINEL) $(MODULES_SENTINEL) | $(OUT_DIR) $(BUILD_DIR)
	javac -source 8 -target 8 -cp $(STUBS_OUT):$(OUT_DIR) -d $(OUT_DIR) $(JAVA_DIR)/RenderHook.java 2>/dev/null
	@touch $@
	@echo "Compiled RenderHook.class"

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
	@echo "Cleaned build artifacts"

install: all
	cp $(INJECTOR) /tmp/
	cp $(PAYLOAD) /tmp/
	cp $(UNLOADER) /tmp/
	cp $(OUT_DIR)/RenderHook.class /tmp/
	mkdir -p /tmp/client/rendering
	cp $(OUT_DIR)/client/*.class /tmp/client/
	cp $(OUT_DIR)/client/rendering/*.class /tmp/client/rendering/
	@echo ""
	@echo "Installed to /tmp. Inject with:"
	@echo "  sudo /tmp/injector <minecraft_pid> /tmp/payload.so"
	@echo "Unload out-of-band with:"
	@echo "  sudo /tmp/unloader <minecraft_pid>"

help:
	@echo "Linux native JVM injector"
	@echo ""
	@echo "Targets: all injector payload unloader java install clean"
	@echo "JAVA_HOME = $(JAVA_HOME)"
	@echo ""
	@echo "Requires GCC, a JDK (JNI headers + javac), and root for ptrace."
