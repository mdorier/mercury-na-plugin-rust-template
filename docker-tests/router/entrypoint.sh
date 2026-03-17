#!/bin/bash
set -e
echo 1 > /proc/sys/net/ipv4/ip_forward

# Find the public-facing interface (172.20.0.x)
PUBLIC_DEV=$(ip -4 -o addr show | grep '172\.20\.0\.' | awk '{print $2}')

# NAT all traffic from internal network out through the public interface
iptables -t nat -A POSTROUTING -o "$PUBLIC_DEV" -j MASQUERADE
iptables -A FORWARD -j ACCEPT

echo "Router: NAT configured on $PUBLIC_DEV"
exec sleep infinity
