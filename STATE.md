# Состояние: RK3566 NPU на mainline 6.18

Дата: 2026-11-16, ночь. Ветка `main`.

## Что работает

- `rknpu` 0.9.8 как модуль на vanilla 6.18.44 (Armbian `current-rockchip64`),
  NPU в **non-IOMMU** режиме: `resnet18` 224×224 — **8.87 мс/кадр, 112.8 к/с**
  при `governor=performance` (800 МГц), выход-отпечаток `first -0.355571`.
- 6.18-compat (devfreq governor, окно частот, vermagic) — вошло в
  `w568w/rknpu-module` (PR #5 смержен 16.11).

## Настоящая причина неработающего IOMMU

Не `GFP_DMA32` (так было написано в issue #1 — неверно). MMU у NPU на RK3566 —
блок **IOMMU v1** (32-битный DTE), а VOP2/VEPU/VDPU — v2 (40 бит). Драйвер
держит один глобальный `rk_ops` и на вторую ревизию отвечает
`WARN_ON(rk_ops != ops); return -EINVAL;`. Поэтому NPU, описанный как
`rockchip,rk3568-iommu` (v2), пробуется и падает на первом задании.

Лечение: per-device ops (Simon Xue, «iommu/rockchip: Drop global rk_ops in favor
of per-device ops», lore `linux-rockchip/20260310105303.128859-1-xxm`) + описание
NPU MMU как `rockchip,iommu` (v1). Апстрим (RFC «accel: rocket: Add RK3568 NPU
support») делает ровно это же.

## Готово к проверке на плате

| файл | что |
|---|---|
| `patches/0002-iommu-per-device-ops.patch` | бэкпорт per-device ops на 6.18.44; дерево собирается без предупреждений |
| `dts/rk3566-rknpu-iommu-overlay.dts` | тот же оверлей, но IOMMU включён и описан как `rockchip,iommu`; метка `npu_mmu` обязательна (иначе dtc не строит fixup для `iommus`) |
| `Image.vanilla` / `Image.perdevops` | ядра на хосте, vermagic `6.18.44-current-rockchip64 SMP preempt mod_unload aarch64` |

Собранные ядра лежат вне репозитория: `~/work/k618/`, dtb с включённым IOMMU —
`~/work/dtcheck/merged.dtb`, набор для карты — `~/work/radxa-sd/sd-root/`.

## Где застряли

`kexec` на этой плате не годится: `kexec -l` и `kexec -l -s` грузятся без ошибки,
а `kexec -e` уводит плату с сети до передёргивания питания (дважды). Загрузка с
microSD не поднялась: плата зажигает диод, коротко мигает и больше не светится —
при этом без карты eMMC грузится штатно, то есть карта перехватывает BootROM.

## Следующий шаг

1. Подключить micro-HDMI экран (есть 5") или USB-UART на `ttyS2` (1500000 8n1) —
   без консоли каждый опыт ядра стоит передёргивания и ничего не объясняет.
2. Поставить ядро с per-device ops и оверлей v1: `scripts/install-perdevops.sh`
   (борт в сети, `BOARD=root@<ip>`), затем `reboot`. Ставится в
   `/boot/vmlinuz-6.18.44-current-rockchip64` — на него смотрит симлинк
   `/boot/Image`, который грузит `boot.scr`; старая пара сохраняется как
   `.orig-<штамп>`, откат — `scripts/rollback-perdevops.sh <штамп>`.
3. Приёмка: `scripts/accept-iommu.sh` — группы IOMMU (NPU внутри), ноль
   `failed with error -22`, живой `fde40000.npu`, 5 прогонов resnet18 по 20
   итераций: ожидаем ~8.9 мс и отпечаток `-0.355571`, как в non-IOMMU режиме.
4. После подтверждения — поправить issue #1 (причина не GFP_DMA32, а ревизия
   MMU) и приложить результат к треду per-device ops как тест на RK3566.

## Состояние на 2026-11-20

Плата снята с хоста (в её слот воткнут диск старого сервера), в LAN её нет:
`lsusb` без `2207:`, TCP/22 не отвечает ни на одном адресе.

Сделано без платы: оверлей v1 слит с настоящим `base.dtb` борта через
`fdtoverlay` — в результате `iommu@fde4b000` с `compatible = "rockchip,iommu"`
и `status = "okay"`, у `npu@fde40000` `iommus` указывает на этот узел (phandle
совпадает). `Image.perdevops` несёт vermagic `6.18.44-current-rockchip64 SMP
preempt mod_unload aarch64`, то есть модули борта (rknpu, aic8800) к нему
подходят. Скрипты установки, отката, приёмки и поиска борта в LAN лежат в
`scripts/`.

Страховка: полный образ eMMC от 19.11 — `~/radxa-emmc-20261119.img.zst` (763 МБ),
файлы борта — `~/radxa-files-20261119.tar.zst`.
