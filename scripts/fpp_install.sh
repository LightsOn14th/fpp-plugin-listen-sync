#!/bin/bash
set -e

# fpp-plugin-listen-sync install script. Safe to run more than once.

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common"

BASEDIR=$(dirname "$0")
cd "$BASEDIR"
cd ..

# The daemon's only dependency (also declared in pluginInfo.json).
if ! python3 -c 'import websockets.asyncio.client' 2>/dev/null; then
    apt-get install -y python3-websockets
fi

make "SRCDIR=${SRCDIR}"

DATA_DIR="${MEDIADIR}/plugindata/fpp-plugin-listen-sync"
mkdir -p "${DATA_DIR}"
chown fpp:fpp "${DATA_DIR}" 2>/dev/null || true

# Restart the daemon so an update takes effect without restarting fppd.
bash scripts/preStop.sh
bash scripts/postStart.sh

# No restartFlag: the Plugin Manager asks fppd to load the plugin as soon as
# this script finishes.
