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
sudo /tmp/injector "$PID" /tmp/payload.so
