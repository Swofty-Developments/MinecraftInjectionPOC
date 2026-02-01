#!/bin/bash
set -e

cd "$(dirname "$0")"

ASM_VERSION="9.6"
ASM_JAR="lib/asm-${ASM_VERSION}.jar"

# Download ASM if not present
if [ ! -f "$ASM_JAR" ]; then
    echo "[*] Downloading ASM ${ASM_VERSION}..."
    curl -sL "https://repo1.maven.org/maven2/org/ow2/asm/asm/${ASM_VERSION}/asm-${ASM_VERSION}.jar" -o "$ASM_JAR"
    echo "[+] Downloaded ASM"
fi

rm -rf build
mkdir -p build/agent build/bootstrap build/injector

# --- Build Agent JAR (targeting Java 8 for MC compatibility) ---
echo "[*] Compiling agent (Java 8 target)..."
javac --release 8 -cp "$ASM_JAR" -d build/agent \
    src/dev/injector/Agent.java \
    src/dev/injector/AgentBootstrap.java \
    src/dev/injector/HookRegistry.java \
    src/dev/injector/stealth/TransformerCoordinator.java \
    src/dev/injector/stealth/DestructHandler.java \
    src/dev/injector/stealth/OverlayTransformer.java

# Shade ASM into the agent JAR (so it's self-contained)
echo "[*] Shading ASM into agent..."
cd build/agent
jar xf "../../$ASM_JAR"
rm -rf META-INF
rm -f module-info.class
cd ../..

echo "[*] Packaging agent.jar (full - for stealth classloader)..."
jar cfm build/agent.jar manifests/MANIFEST-AGENT.MF -C build/agent .

# --- Build bootstrap.jar (minimal - only bootstrap classes, NO stealth package) ---
echo "[*] Packaging bootstrap.jar (minimal - for bootstrap classloader)..."
mkdir -p build/bootstrap/dev/injector
cp build/agent/dev/injector/Agent.class build/bootstrap/dev/injector/
cp build/agent/dev/injector/AgentBootstrap.class build/bootstrap/dev/injector/
cp build/agent/dev/injector/HookRegistry.class build/bootstrap/dev/injector/
jar cf build/bootstrap.jar -C build/bootstrap .

# --- Build Injector JAR (runs on host JDK, doesn't need Java 8) ---
echo "[*] Compiling injector..."
javac -d build/injector \
    src/dev/injector/Injector.java

echo "[*] Packaging injector.jar..."
jar cfm build/injector.jar manifests/MANIFEST-INJECTOR.MF -C build/injector .

echo ""
echo "[+] Build complete!"
echo "    build/injector.jar   - Run this to inject into Minecraft"
echo "    build/agent.jar      - Full agent (loaded by stealth classloader)"
echo "    build/bootstrap.jar  - Minimal bootstrap classes only"
echo ""
echo "Usage:"
echo "  1. Start Minecraft 1.8.9"
echo "  2. Run: ./inject.sh"
