#!/bin/bash

# Start the Listen Sync daemon in the background. Returns immediately.

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common"

PLUGIN_DIR=$(cd "$(dirname "$0")/.." && pwd)
DATA_DIR="${MEDIADIR}/plugindata/fpp-plugin-listen-sync"
PID_FILE="${DATA_DIR}/daemon.pid"
LOG_FILE="${LOGDIR}/plugin-fpp-plugin-listen-sync.log"

mkdir -p "${DATA_DIR}"

if [ -f "${PID_FILE}" ] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
    exit 0
fi

# setsid gives the runner its own process group so preStop.sh can stop the
# runner and the daemon together.
MEDIADIR="${MEDIADIR}" LOGDIR="${LOGDIR}" setsid nohup "${PLUGIN_DIR}/scripts/listen_sync_run.sh" >> "${LOG_FILE}" 2>&1 < /dev/null &
echo $! > "${PID_FILE}"
