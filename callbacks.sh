#!/bin/bash

# Tells fppd this is a C++ plugin so it loads libfpp-plugin-listen-sync.so.
for var in "$@"
do
    case $var in
        -l|--list)
            echo "c++"
            exit 0
        ;;
        *)
            exit 0
        ;;
    esac
done
