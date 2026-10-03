#!/bin/bash

# Keeps listen_sync_daemon.py running: restarts it 5 seconds after any exit.
# Started by postStart.sh in its own process group, stopped by preStop.sh.

PLUGIN_DIR=$(cd "$(dirname "$0")/.." && pwd)

while true
do
    python3 "${PLUGIN_DIR}/listen_sync_daemon.py"
    STATUS=$?
    echo "$(date '+%Y-%m-%d %H:%M:%S') daemon exited with status ${STATUS}; restarting in 5 s"
    sleep 5
done
