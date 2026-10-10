#!/bin/sh
# Hook for `mifmon -d -s rmnet-up.sh`: configure the rmnet interface of a
# data call once the CP reports its IP configuration.
# Environment (set by mifmon): IFACE CID IP DNS1 DNS2 MTU
#
# The default route goes in with a high metric so Wi-Fi/USB networking keeps
# priority; set RMNET_METRIC=0 to make mobile data the preferred route.
# DNS is written to /run/rmnet-resolv.conf only (not /etc/resolv.conf).
set -eu

metric=${RMNET_METRIC:-700}

ip addr flush dev "$IFACE"
ip addr add "$IP/32" dev "$IFACE"
[ "${MTU:-0}" -gt 0 ] && ip link set "$IFACE" mtu "$MTU"
ip link set "$IFACE" up
ip route replace default dev "$IFACE" metric "$metric"

printf 'nameserver %s\nnameserver %s\n' "$DNS1" "$DNS2" > /run/rmnet-resolv.conf
logger -t rmnet-up "$IFACE up: cid $CID mtu $MTU" 2>/dev/null || true
