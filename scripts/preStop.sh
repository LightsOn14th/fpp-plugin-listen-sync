#!/bin/bash

# Stop the Listen Sync daemon. Safe to run when it is not running.

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common"

PID_FILE="${MEDIADIR}/plugindata/fpp-plugin-listen-sync/daemon.pid"

if [ -f "${PID_FILE}" ]; then
    PID=$(cat "${PID_FILE}")

    if [ -n "${PID}" ] && kill -0 "${PID}" 2>/dev/null; then
        kill -TERM -- "-${PID}" 2>/dev/null || kill -TERM "${PID}" 2>/dev/null || true
    fi

    rm -f "${PID_FILE}"
fi

exit 0
