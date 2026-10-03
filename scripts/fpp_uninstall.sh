#!/bin/bash

# fpp-plugin-listen-sync uninstall script. Safe to run more than once.

: "${FPPDIR:=/opt/fpp}"
. "${FPPDIR}/scripts/common"

bash "$(dirname "$0")/preStop.sh"

# The relay token, status file and daemon pid are the only things kept outside
# the plugin directory.
rm -rf "${MEDIADIR}/plugindata/fpp-plugin-listen-sync"

# No restartFlag: the Plugin Manager unloads the plugin through fppd before it
# removes these files.
