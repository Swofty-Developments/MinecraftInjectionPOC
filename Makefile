# Makefile for Linux Native JVM Injector
#
# Usage:
#   make              - Build everything
#   make injector     - Build just the injector
#   make payload      - Build just the payload .so
#   make java         - Compile Bootstrap.java
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
BUILD_DIR = build
OUT_DIR = out

# Targets
INJECTOR = $(OUT_DIR)/injector
PAYLOAD = $(OUT_DIR)/payload.so
UNLOADER = $(OUT_DIR)/unloader
BOOTSTRAP_CLASS = $(OUT_DIR)/Bootstrap.class

.PHONY: all clean install injector payload unloader java help

all: $(INJECTOR) $(PAYLOAD) $(UNLOADER) $(BOOTSTRAP_CLASS)

# Create directories
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

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

# Compile Java
java: $(BOOTSTRAP_CLASS)

$(BOOTSTRAP_CLASS): $(JAVA_DIR)/Bootstrap.java | $(OUT_DIR)
	javac -source 8 -target 8 -d $(OUT_DIR) $<
	@echo "Compiled Bootstrap.class: $@"

# Clean
clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
	@echo "Cleaned build artifacts"

# Install to /tmp for testing
install: all
	cp $(INJECTOR) /tmp/
	cp $(PAYLOAD) /tmp/
	cp $(UNLOADER) /tmp/
	cp $(BOOTSTRAP_CLASS) /tmp/
	@echo ""
	@echo "Installed to /tmp:"
	@echo "  /tmp/injector"
	@echo "  /tmp/payload.so"
	@echo "  /tmp/unloader"
	@echo "  /tmp/Bootstrap.class"
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
	@echo "  java      - Compile Bootstrap.java"
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
