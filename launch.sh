#!/bin/bash
cd "$(dirname "$(readlink -f "$0")")" || exit 1
export LD_LIBRARY_PATH="$PWD/lib:$LD_LIBRARY_PATH"
./Lithium_Game project.lithium
