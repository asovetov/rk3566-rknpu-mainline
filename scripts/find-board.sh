#!/bin/bash
# Ищет борт в LAN: перебирает TCP/22 и показывает, где отвечает наш ключ.
set -uo pipefail

KEY=${KEY:-$HOME/.ssh/radxa_ed25519}
NET=${NET:-192.168.0}
USER_=${USER_:-root}

for i in $(seq 1 254); do
  ip="$NET.$i"
  timeout 1 bash -c "exec 3<>/dev/tcp/$ip/22" 2>/dev/null || continue
  banner=$(timeout 2 bash -c "exec 3<>/dev/tcp/$ip/22; head -c 40 <&3" 2>/dev/null | tr -d '\0\r')
  key=""
  if ssh -i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
        "$USER_@$ip" true 2>/dev/null; then key="НАШ-КЛЮЧ-ОК"; fi
  printf '%-15s %-45s %s\n' "$ip" "${banner:-?}" "$key"
done
