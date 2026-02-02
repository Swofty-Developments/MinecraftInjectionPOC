#!/bin/bash
PID=$(pgrep -f 'java.*minecraft' | head -1)
if [ -z "$PID" ]; then
    PID=$(pgrep java | head -1)
fi
if [ -z "$PID" ]; then
    echo "No Minecraft/Java process found"
    exit 1
fi
echo "Found Minecraft PID: $PID"

# Copy payload.so to a unique temp file each time.
# After unload, munmap removes the .so pages but glibc's internal link_map
# still has an entry for the old path. dlopen with the same path just bumps
# the refcount without calling the constructor. A unique path forces glibc
# to treat it as a brand new library.
rm -f /tmp/payload_inject_*.so 2>/dev/null
UNIQUE="/tmp/payload_inject_$$.so"
cp /tmp/payload.so "$UNIQUE"

echo "Injecting $UNIQUE"
sudo /tmp/injector "$PID" "$UNIQUE"
