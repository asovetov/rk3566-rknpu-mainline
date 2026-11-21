#!/bin/bash
# Ставит на борт собранные Armbian deb-пакеты ядра (linux-image/linux-dtb/linux-headers).
# Перед установкой снимает копию /boot и списка модулей, чтобы откат был одним dpkg -i.
set -euo pipefail

BOARD=${BOARD:-root@192.168.0.70}
KEY=${KEY:-$HOME/.ssh/radxa_ed25519}
DEBS=${DEBS:-$HOME/proj/armbian-build/output/debs}
REL=${REL:-6.18.24-current-rockchip64}

SSH="ssh -i $KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
SCP="scp -i $KEY -o StrictHostKeyChecking=accept-new"
STAMP=$(date +%Y%m%d-%H%M)

die() { echo "СТОП: $*" >&2; exit 1; }

# shellcheck disable=SC2012
mapfile -t PKGS < <(ls "$DEBS"/linux-image-"$REL"_*.deb "$DEBS"/linux-dtb-"$REL"_*.deb 2>/dev/null)
[ "${#PKGS[@]}" -ge 2 ] || die "не нашёл linux-image/linux-dtb для $REL в $DEBS"

echo "== пакеты: ${PKGS[*]##*/}"
echo "== 1/4 снимок /boot и модулей на борту"
$SSH "$BOARD" "set -e
  tar -C / -cf /root/boot-backup-$STAMP.tar boot lib/modules/$(uname -r)
  cp -a /boot/vmlinuz-$REL /boot/vmlinuz-$REL.pre-debs-$STAMP 2>/dev/null || true
  ls -la /root/boot-backup-$STAMP.tar"

echo "== 2/4 копирую пакеты"
$SCP "${PKGS[@]}" "$BOARD:/root/"

echo "== 3/4 установка"
$SSH "$BOARD" "set -e
  DEBIAN_FRONTEND=noninteractive dpkg -i $(for p in "${PKGS[@]}"; do printf '/root/%s ' "$(basename "$p")"; done)
  depmod -a $(echo "$REL" | sed 's/-current-rockchip64//')-current-rockchip64 || true
  ls -la /boot/vmlinuz-$REL /boot/Image
  readlink -f /boot/Image
  ls /lib/modules/ | tail -3"

echo "== 4/4 сверка vermagic"
$SSH "$BOARD" "uname -r; grep -a -o '6\.18\.24-current-rockchip64 SMP preempt mod_unload aarch64' /boot/vmlinuz-$REL | head -1 || echo 'vermagic не найден в файле'"

echo "готово. Перезагрузка: $SSH $BOARD reboot"
