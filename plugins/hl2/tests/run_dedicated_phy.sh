#!/bin/bash
# Run from the repository root. Retain each failed attempt separately when investigating.
set -eu
mkdir -p rig/lobbyfast
cc -std=gnu11 -DFTEPLUGIN -DFTE_TLS=__thread \
  -Iplugins -Iengine/client -Iengine/common -Iengine/server -Iengine/gl -Iengine/qclib \
  -ffunction-sections -fdata-sections plugins/hl2/tests/dedicated_phy.c \
  -Wl,--gc-sections -lm -o rig/lobbyfast/dedicated_phy
rig/lobbyfast/dedicated_phy
