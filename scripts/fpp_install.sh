#!/bin/bash
set -e

# fpp-plugin-listen-sync install script. Safe to run more than once.

BASEDIR=$(dirname "$0")
cd "$BASEDIR"
cd ..
make "SRCDIR=${SRCDIR}"

# No restartFlag: the Plugin Manager asks fppd to load the plugin as soon as
# this script finishes.
