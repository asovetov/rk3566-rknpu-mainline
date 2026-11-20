#!/bin/bash
# Ставит на Radxa Zero 3W ядро с per-device ops IOMMU и оверлей с MMU как rockchip,iommu (v1).
# Старое ядро и оверлей сохраняются рядом с суффиксом .orig-<штамп>.
set -euo pipefail

BOARD=${BOARD:-root@192.168.0.70}
KEY=${KEY:-$HOME/.ssh/radxa_ed25519}
REL=${REL:-6.18.44-current-rockchip64}
IMAGE=${IMAGE:-$HOME/work/k618/Image.perdevops}
OVERLAY=${OVERLAY:-$HOME/proj/rknpu-iommu/rknpu-iommu-v1-overlay.dtbo}

SSH="ssh -i $KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
SCP="scp -i $KEY -o StrictHostKeyChecking=accept-new"
STAMP=$(date +%Y%m%d-%H%M)

die() { echo "СТОП: $*" >&2; exit 1; }

[ -f "$IMAGE" ]   || die "нет ядра $IMAGE"
[ -f "$OVERLAY" ] || die "нет оверлея $OVERLAY"

# vermagic должен совпасть с релизом борта, иначе модули (rknpu, aic8800) не загрузятся
grep -aq "$REL SMP preempt mod_unload aarch64" "$IMAGE" || die "vermagic в $IMAGE не $REL"

# в оверлее MMU обязан быть v1, иначе вернётся глобальный rk_ops и блоки v2 отвалятся
dtc -I dtb -O dts "$OVERLAY" 2>/dev/null | grep -q 'compatible = "rockchip,iommu"' \
    || die "оверлей не описывает MMU как rockchip,iommu (v1)"

echo "== борт: $BOARD, ядро $REL"
$SSH "$BOARD" "uname -r; readlink -f /boot/Image; cat /proc/device-tree/model; echo"

echo "== 1/4 бэкап ядра и оверлея на борту"
$SSH "$BOARD" "set -e
  cp -a /boot/vmlinuz-$REL /boot/vmlinuz-$REL.orig-$STAMP
  cp -a /boot/overlay-user/rk3566-rknpu.dtbo /boot/overlay-user/rk3566-rknpu.dtbo.orig-$STAMP
  ls -la /boot/vmlinuz-$REL.orig-$STAMP /boot/overlay-user/rk3566-rknpu.dtbo.orig-$STAMP"

echo "== 2/4 копирую ядро и оверлей"
$SCP "$IMAGE" "$BOARD:/boot/vmlinuz-$REL.new"
$SCP "$OVERLAY" "$BOARD:/boot/overlay-user/rk3566-rknpu.dtbo.new"

echo "== 3/4 сверяю контрольные суммы"
L_IMG=$(md5sum "$IMAGE" | cut -d' ' -f1)
L_OVL=$(md5sum "$OVERLAY" | cut -d' ' -f1)
R_IMG=$($SSH "$BOARD" "md5sum /boot/vmlinuz-$REL.new | cut -d' ' -f1")
R_OVL=$($SSH "$BOARD" "md5sum /boot/overlay-user/rk3566-rknpu.dtbo.new | cut -d' ' -f1")
[ "$L_IMG" = "$R_IMG" ] || die "ядро доехало битым: $L_IMG != $R_IMG"
[ "$L_OVL" = "$R_OVL" ] || die "оверлей доехал битым: $L_OVL != $R_OVL"
echo "суммы совпали: $L_IMG / $L_OVL"

echo "== 4/4 подмена и sync"
$SSH "$BOARD" "set -e
  mv /boot/vmlinuz-$REL.new /boot/vmlinuz-$REL
  mv /boot/overlay-user/rk3566-rknpu.dtbo.new /boot/overlay-user/rk3566-rknpu.dtbo
  sync
  ls -la /boot/vmlinuz-$REL /boot/overlay-user/rk3566-rknpu.dtbo; readlink -f /boot/Image"

echo "готово. Откат: scripts/rollback-perdevops.sh $STAMP"
echo "перезагрузка: $SSH $BOARD reboot"
