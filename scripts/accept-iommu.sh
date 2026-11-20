#!/bin/bash
# Приёмка IOMMU-режима на борту: состояние ядра, группы IOMMU и 5 прогонов resnet18.
# Запускать после перезагрузки: после job timeout NPU уходит в soft reset и прогоны врут.
set -euo pipefail

BOARD=${BOARD:-root@192.168.0.70}
KEY=${KEY:-$HOME/.ssh/radxa_ed25519}
MODEL=${MODEL:-/root/resnet18_for_rk3566_rk3568.rknn}
BENCH=${BENCH:-/root/rknn_bench}

ssh -i "$KEY" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "$BOARD" 'bash -s' -- "$MODEL" "$BENCH" <<'EOF'
set -u
MODEL=$1; BENCH=$2
echo "== ядро: $(uname -r)"
echo "== модель платы: $(tr -d '\0' < /proc/device-tree/model)"
echo "== IOMMU-группы:"
for g in /sys/kernel/iommu_groups/*; do
  [ -d "$g" ] || { echo "  нет групп"; break; }
  printf '  %s: %s\n' "$(basename "$g")" "$(cat "$g"/devices/* 2>/dev/null | tr '\n' ' ')"
done
echo "== dmesg: отказы проб v2-блоков (ждать 0): $(dmesg | grep -c 'failed with error -22' || true)"
dmesg | grep -iE 'rk_iommu|rknpu' | tail -12
echo "== rknpu: $(cat /sys/kernel/debug/rknpu/version 2>/dev/null || echo 'нет debugfs-версии')"
if [ -d /sys/class/devfreq/fde40000.npu ]; then
  echo performance > /sys/class/devfreq/fde40000.npu/governor 2>/dev/null || true
  echo "== devfreq: governor=$(cat /sys/class/devfreq/fde40000.npu/governor) cur=$(cat /sys/class/devfreq/fde40000.npu/cur_freq)"
else
  echo "== devfreq: нет узла fde40000.npu (битый оверлей или NPU не поднялся)"
fi
[ -x "$BENCH" ] || { echo "нет $BENCH"; exit 1; }
[ -f "$MODEL" ] || { echo "нет $MODEL"; exit 1; }
echo "== 5 прогонов по 20 итераций:"
for i in 1 2 3 4 5; do
  printf '  #%d ' "$i"
  "$BENCH" "$MODEL" 20 2>&1 | grep -E 'first output|per-frame' | tr '\n' ' '
  echo
done
echo "== dvfs-ошибки (ждать 0): $(dmesg | grep -c 'dvfs failed' || true)"
EOF
