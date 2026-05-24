#!/usr/bin/env bash
# macOS bring-up: configure the utun device that usertcp created.
# The kernel assigns the utun number when the program connects to its
# control socket, so you must start usertcp first, note the utunN name
# it prints, then run this script in another shell.
#
# Usage: ./scripts/setup-macos.sh utunN [local] [peer]
#   utunN  the device name printed by usertcp (e.g. utun5)
#   local  local address (default: 10.0.0.1)
#   peer   peer address  (default: 10.0.0.2)

set -eu

if [[ -z "${1:-}" || "${1:-}" == "utunN" ]]; then
  echo "usage: $0 <utunN> [local] [peer]" >&2
  echo "       start ./usertcp first; copy the utunN name it prints." >&2
  exit 1
fi

IF=$1
LOCAL=${2:-10.0.0.1}
PEER=${3:-10.0.0.2}

sudo ifconfig "$IF" "$LOCAL" "$PEER" up
sudo route -nq add -net 10.0.0.0/24 -interface "$IF" 2>/dev/null || true

echo "interface: $IF"
echo "local:     $LOCAL"
echo "peer:      $PEER"
echo "try:       ping $PEER"
