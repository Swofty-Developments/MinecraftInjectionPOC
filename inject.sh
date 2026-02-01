#!/bin/bash
set -e

cd "$(dirname "$0")"

INJECTOR="build/injector.jar"
AGENT="$(pwd)/build/agent.jar"

if [ ! -f "$INJECTOR" ] || [ ! -f "$AGENT" ]; then
    echo "[!] JARs not found. Run ./build.sh first."
    exit 1
fi

echo "[*] Injecting into Minecraft..."
java --add-opens java.base/java.lang=ALL-UNNAMED \
     -cp "$INJECTOR" \
     dev.injector.Injector "$AGENT" "$@"
