#!/bin/bash
# Установка rknpu 0.9.8 + DT-оверлея на Armbian (mainline 6.18, Radxa Zero 3W).
# Запускать на плате, из каталога с rknpu.ko и rk3566-rknpu.dtbo.
set -eu
KO=${1:-rknpu.ko}
DTBO=${2:-rk3566-rknpu.dtbo}
KREL=$(uname -r)

install -m 644 "$KO" "/lib/modules/$KREL/updates/rknpu.ko"
depmod -a
mkdir -p /boot/overlay-user
install -m 644 "$DTBO" /boot/overlay-user/rk3566-rknpu.dtbo
grep -q 'user_overlays=.*rk3566-rknpu' /boot/armbianEnv.txt || \
	echo 'user_overlays=rk3566-rknpu' >> /boot/armbianEnv.txt
echo "готово: перезагрузите плату (reboot)"
