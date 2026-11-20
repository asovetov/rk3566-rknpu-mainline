#!/bin/bash
# Откатывает ядро и оверлей к версиям .orig-<штамп>, снятым install-perdevops.sh.
set -euo pipefail

BOARD=${BOARD:-root@192.168.0.70}
KEY=${KEY:-$HOME/.ssh/radxa_ed25519}
REL=${REL:-6.18.44-current-rockchip64}
STAMP=${1:?укажи штамп: scripts/rollback-perdevops.sh 20261120-1900}
SSH="ssh -i $KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"

$SSH "$BOARD" "set -e
  test -e /boot/vmlinuz-$REL.orig-$STAMP || { echo 'нет копии ядра'; exit 1; }
  cp -a /boot/vmlinuz-$REL.orig-$STAMP /boot/vmlinuz-$REL
  cp -a /boot/overlay-user/rk3566-rknpu.dtbo.orig-$STAMP /boot/overlay-user/rk3566-rknpu.dtbo
  sync
  ls -la /boot/vmlinuz-$REL /boot/overlay-user/rk3566-rknpu.dtbo"
echo "откат на $STAMP сделан, борт перезагружать: $SSH $BOARD reboot"
