#!/usr/bin/env bash
# Linux one-time setup: create a persistent TUN device owned by the
# current user, assign it an IP, and bring it up. Idempotent (errors
# from already-existing state are tolerated).
#
# Usage: ./scripts/setup-linux.sh [ifname] [cidr]
#   ifname  device name (default: tun0)
#   cidr    local address with prefix (default: 10.0.0.1/24)
#
# After this you can run `./usertcp tun0` without sudo. Send traffic
# from another shell with `ping 10.0.0.2`.

set -eu

IF=${1:-tun0}
CIDR=${2:-10.0.0.1/24}

sudo ip tuntap add dev "$IF" mode tun user "$USER" 2>/dev/null || true
sudo ip addr add "$CIDR" dev "$IF" 2>/dev/null || true
sudo ip link set "$IF" up

echo "tun device: $IF"
echo "address:    $CIDR"
echo "try:        ping 10.0.0.2"
