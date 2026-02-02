# Makefile for Linux Native JVM Injector
#
# Usage:
#   make              - Build everything
#   make injector     - Build just the injector
#   make payload      - Build just the payload .so
#   make java         - Compile Java classes
#   make clean        - Remove build artifacts
#   make install      - Copy files to /tmp for easy testing

# Detect JAVA_HOME
JAVA_HOME ?= $(shell dirname $(shell dirname $(shell readlink -f $(shell which java))))
ifeq ($(JAVA_HOME),)
    JAVA_HOME = /usr/lib/jvm/java-8-openjdk-amd64
endif

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -ldl

# JNI settings
JNI_INCLUDE = -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux

# Directories
SRC_DIR = src
JAVA_DIR = java
STUBS_DIR = stubs
BUILD_DIR = build
OUT_DIR = out
STUBS_OUT = $(BUILD_DIR)/stubs

# Native targets
INJECTOR = $(OUT_DIR)/injector
PAYLOAD = $(OUT_DIR)/payload.so
UNLOADER = $(OUT_DIR)/unloader

# Java sentinel files (javac produces many .class files per stage)
STUBS_SENTINEL = $(BUILD_DIR)/.stubs_built
MODULES_SENTINEL = $(BUILD_DIR)/.modules_built
HOOK_SENTINEL = $(BUILD_DIR)/.hook_built

# All Java source files
CLIENT_SOURCES = $(wildcard $(JAVA_DIR)/client/*.java)
RENDERING_SOURCES = $(wildcard $(JAVA_DIR)/client/rendering/*.java)
ALL_MODULE_SOURCES = $(CLIENT_SOURCES) $(RENDERING_SOURCES)

.PHONY: all clean install injector payload unloader java help

all: $(INJECTOR) $(PAYLOAD) $(UNLOADER) $(HOOK_SENTINEL)

# Create directories
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(STUBS_OUT):
	mkdir -p $(STUBS_OUT)

# Build injector
injector: $(INJECTOR)

$(INJECTOR): $(SRC_DIR)/injector.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built injector: $@"

# Build payload .so
payload: $(PAYLOAD)

$(PAYLOAD): $(SRC_DIR)/payload.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< $(JNI_INCLUDE) $(LDFLAGS) -lpthread
	@echo "Built payload: $@"

# Build unloader
unloader: $(UNLOADER)

$(UNLOADER): $(SRC_DIR)/unloader.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built unloader: $@"

# Compile Java in stages:
#   1. MC stubs (ave, avo) to build/stubs/
#   2. All client module classes to out/
#   3. RenderHook (needs stubs + client) to out/
java: $(HOOK_SENTINEL)

# Stage 1: MC class stubs (compile-time only, never deployed)
$(STUBS_SENTINEL): $(STUBS_DIR)/ave.java $(STUBS_DIR)/avo.java | $(STUBS_OUT) $(BUILD_DIR)
	javac -source 8 -target 8 -d $(STUBS_OUT) $(STUBS_DIR)/ave.java $(STUBS_DIR)/avo.java 2>/dev/null
	@touch $@
	@echo "Compiled MC stubs"

# Stage 2: All client + rendering classes (compiled together for cross-references)
$(MODULES_SENTINEL): $(ALL_MODULE_SOURCES) | $(OUT_DIR) $(BUILD_DIR)
	javac -source 8 -target 8 -d $(OUT_DIR) $(ALL_MODULE_SOURCES) 2>/dev/null
	@touch $@
	@echo "Compiled client modules"

# Stage 3: RenderHook (default package, depends on stubs + client modules)
$(HOOK_SENTINEL): $(JAVA_DIR)/RenderHook.java $(STUBS_SENTINEL) $(MODULES_SENTINEL) | $(OUT_DIR) $(BUILD_DIR)
	javac -source 8 -target 8 -cp $(STUBS_OUT):$(OUT_DIR) -d $(OUT_DIR) $(JAVA_DIR)/RenderHook.java 2>/dev/null
	@touch $@
	@echo "Compiled RenderHook.class"

# Clean
clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
	@echo "Cleaned build artifacts"

# Install to /tmp for testing
install: all
	cp $(INJECTOR) /tmp/
	cp $(PAYLOAD) /tmp/
	cp $(UNLOADER) /tmp/
	cp $(OUT_DIR)/RenderHook.class /tmp/
	mkdir -p /tmp/client/rendering
	cp $(OUT_DIR)/client/*.class /tmp/client/
	cp $(OUT_DIR)/client/rendering/*.class /tmp/client/rendering/
	@echo ""
	@echo "Installed to /tmp:"
	@echo "  /tmp/injector"
	@echo "  /tmp/payload.so"
	@echo "  /tmp/unloader"
	@echo "  /tmp/RenderHook.class"
	@echo "  /tmp/client/*.class"
	@echo "  /tmp/client/rendering/*.class"
	@echo ""
	@echo "Usage:"
	@echo "  sudo /tmp/injector <minecraft_pid> /tmp/payload.so"
	@echo ""
	@echo "To unload:"
	@echo "  sudo /tmp/unloader <minecraft_pid>"

# Help
help:
	@echo "Linux Native JVM Injector"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build everything (default)"
	@echo "  injector  - Build the ptrace injector"
	@echo "  payload   - Build the payload .so"
	@echo "  unloader  - Build the unloader"
	@echo "  java      - Compile Java classes"
	@echo "  clean     - Remove build artifacts"
	@echo "  install   - Copy files to /tmp"
	@echo ""
	@echo "Environment:"
	@echo "  JAVA_HOME = $(JAVA_HOME)"
	@echo ""
	@echo "Requirements:"
	@echo "  - GCC"
	@echo "  - JDK (for JNI headers and javac)"
	@echo "  - Root access (for ptrace)"
