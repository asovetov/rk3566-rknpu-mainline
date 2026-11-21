# RK3566 NPU на mainline-ядре 6.18 (Armbian current-rockchip64)

Рабочий рецепт: NPU Rockchip (RKNPU2) работает на **ванильном ядре 6.18.44**, без
вендорского BSP. Проверено на Radxa Zero 3W (RK3566).

## Результат

`resnet18` (224×224×3 → 1000), 30 прогонов на живой плате:

| governor | частота NPU | на кадр | FPS | загрузка NPU |
|---|---|---|---|---|
| `performance` | 800 МГц | **9.08 мс** | **110.2** | 88% |
| `simple_ondemand` | 200 МГц (не успел разогнаться) | 34.60 мс | 28.9 | 88% |

DVFS реально переключает частоту: `devfreq` показывает диапазон
200/297/400/600/700/800 МГц, `cur_freq` меняется, `debugfs` живой —
`RKNPU driver: v0.9.8`.

## Почему это не «просто собрать свежее ядро»

- В mainline **нет** драйвера NPU для RK3566: встроенный `accel/rocket`
  поддерживает только RK3588 и говорит на другом ABI (DRM accel), а
  `librknnrt.so` требует вендорский `/dev/rknpu` с его ioctl.
- Поэтому нужен out-of-tree модуль. База: [w568w/rknpu-module](https://github.com/w568w/rknpu-module)
  (порт вендорского `rknpu` 0.9.8 под mainline, тестирован на 6.19/7.0).

## Что пришлось поправить под 6.18 (всё в `patches/0001-6.18-compat.patch`)

1. **`linux/devfreq-governor.h` появился только в 6.19.** Добавлен compat-shim
   (`src/include/compat/linux/devfreq-governor.h`): на ≤6.18 включается
   `linux/devfreq.h`.
2. **`devfreq_add_governor()` не экспортируется модулям до 6.19.** Кастомный
   governor `rknpu_ondemand` оставлен только для 6.19+, на 6.18 используется
   встроенный `simple_ondemand`.
3. **`df->data` для `simple_ondemand`.** Драйвер передавал указатель на
   `rknpu_device`, а governor читает это как `struct devfreq_simple_ondemand_data`
   — мусор в `upthreshold`/`downdifferential`, и каждые 50 мс в лог летит
   `devfreq ...: dvfs failed with (-22) error` (сотни строк, заваливает консоль).
   На 6.18 передаём `NULL`.
4. **Не задавался диапазон частот devfreq.** Без него `simple_ondemand` просит
   0 Гц → `devfreq_recommended_opp()` → `-EINVAL` и тот же спам. Теперь диапазон
   берётся из OPP-таблицы (`devfreq range: 200000000-800000000 Hz` в логе).
5. **`vermagic`.** При сборке против скопированных заголовков `make prepare`
   перегенерирует `utsrelease.h` и модуль получает `6.18.44` вместо
   `6.18.44-current-rockchip64` (не грузится, «version magic ... should be»).
   Лечится правкой `include/generated/utsrelease.h` в каталоге заголовков и
   пересборкой `rknpu.mod.o`.

Оверлей тоже правился (`dts/rk3566-rknpu-overlay.dts`):

- `SCMI_CLK_NPU` для RK356x = **2** (в заголовках 6.18 есть только rk3588-шная
  `= 6`; вендорский `rk356x.dtsi` использует `<&scmi_clk 2>` — сверено с
  вендорским dtb);
- питание NPU на этой плате — `vdd_gpu_npu` (DCDC_REG2 RK809, общий с GPU), а не
  `vdd_npu`;
- добавлен `interrupt-parent = <&gic>` в ноды NPU и IOMMU;
- в репо два варианта оверлея: `dts/rk3566-rknpu-overlay.dts` держит IOMMU
  `disabled`, `dts/rk3566-rknpu-iommu-overlay.dts` включает его как
  `rockchip,iommu` (v1, 32-битный DTE). Второй требует патча ядра, см. ниже.

## Режим IOMMU (работает, нужен патч ядра)

MMU у NPU на RK3566 это блок `rockchip-iommu` **v1** (32-битный DTE), а VOP2, VEPU
и VDPU в том же SoC относятся к v2 (40 бит). Драйвер mainline держит один глобальный `rk_ops`
и отвергает второй экземпляр с другой ревизией
(`WARN_ON(rk_ops != ops); return -EINVAL;`), поэтому v1 и v2 не сосуществуют: чтобы
получить MMU у NPU, приходится гасить дисплей и аппаратный декод.

`patches/0002-iommu-per-device-ops.patch` переносит на 6.18.24 патч Simon Xue
«iommu/rockchip: Drop global rk_ops in favor of per-device ops»: `rk_ops` переезжает
в `struct rk_iommu` и `struct rk_iommu_domain` и ставится из
`of_device_get_match_data()`. После него MMU NPU описан как `rockchip,iommu`, а
остальные блоки остаются на `rockchip,rk3568-iommu`.

Проверено на Radxa ZERO 3W, 6.18.24, все 4 ГБ RAM, rknpu 0.9.8, librknnrt 2.5.0:

```
platform fde40000.npu: Adding to iommu group 0
RKNPU fde40000.npu: RKNPU: rknpu iommu is enabled, using iommu mode
[drm] Initialized rknpu 0.9.8 for fde40000.npu on minor 1
```

resnet18, пять прогонов по 30 итераций на 800 МГц: 9,08, 9,08, 9,09, 9,10, 9,08 мс
на кадр. Отпечаток выхода -0.355571, ошибок `-22` в dmesg нет, таймаутов заданий нет.
Готовые модуль и оверлей лежат в `artifacts/`.

При сборке ядра под образ Armbian есть отдельная ловушка, к NPU отношения не имеющая:
модули в образе несут split BTF, привязанный к id типов своего vmlinux, поэтому
любое изменение конфига ломает `btf_parse_module()` (`-EINVAL`) и `init_module()`
отвергает модуль. Плата при этом грузится без Wi-Fi, а в dmesg 273 строки
`BPF: Invalid name_offset`. Ставится `CONFIG_MODULE_ALLOW_BTF_MISMATCH=y`, и
несовпадение становится предупреждением.

## Сборка

Нужны заголовки целевого ядра и aarch64-кросс-компилятор.

```bash
# заголовки: копируем с платы (или из образа) — они собраны на aarch64,
# поэтому host-утилиты надо пересобрать под x86:
cp -a /mnt/armbian/usr/src/linux-headers-$(uname -r) /tmp/kheaders   # с образом/по ssh
cd /tmp/kheaders
python3 -c "import pathlib;[p.unlink() for p in pathlib.Path('scripts').rglob('*.o')]"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- HOSTCC=gcc HOSTLD=ld prepare scripts
# modpost в заголовках тоже arm64 — собираем вручную:
cd scripts/mod && gcc -O2 -Wall -I. -I../../tools/include -I../../scripts/include \
    -o modpost modpost.c file2alias.c sumversion.c symsearch.c

# модуль
cd rknpu-module && make KDIR=/tmp/kheaders ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# проверка: modinfo rknpu.ko | grep vermagic  → 6.18.44-current-rockchip64 ...

# оверлей
make dtbo KDIR=/tmp/kheaders
fdtoverlay -i /boot/dtb/rockchip/rk3566-radxa-zero-3w.dtb -o /tmp/check.dtb dts/rk3566-rknpu.dtbo
```

## Установка на плату

```bash
install -m644 rknpu.ko /lib/modules/$(uname -r)/updates/ && depmod -a
mkdir -p /boot/overlay-user && cp rk3566-rknpu.dtbo /boot/overlay-user/
echo 'user_overlays=rk3566-rknpu' >> /boot/armbianEnv.txt
reboot
```

Проверка:

```bash
dmesg | grep -i rknpu
# RKNPU: rknpu iommu is enabled, using iommu mode
# [drm] Initialized rknpu 0.9.8 for fde40000.npu on minor 1
# devfreq range: 200000000-800000000 Hz
ls /dev/dri/by-path/ | grep npu      # platform-fde40000.npu-render
cat /sys/kernel/debug/rknpu/version  # RKNPU driver: v0.9.8
```

Для максимума производительности (трекеру важна задержка, а не экономия):

```bash
echo performance > /sys/class/devfreq/fde40000.npu/governor
```

С `simple_ondemand` короткие burst'ы остаются на 200 МГц — governor не успевает
поднять частоту, поэтому 34.6 мс против 9.1 мс.

## Инференс-тест без Python на плате

`scripts/rknn_bench.c` — минимальный C-бенчмарк на `librknnrt.so`
(кросс-компилируется `aarch64-linux-gnu-gcc`), модель берётся готовая
(`resnet18_for_rk3566_rk3568.rknn` из `rknn-toolkit2`).

```bash
LD_LIBRARY_PATH=/usr/lib ./rknn_bench resnet18_for_rk3566_rk3568.rknn 30
# librknnrt: 2.5.0 / driver: 0.9.8
# iters: 30  per-frame: 9.08 ms  fps: 110.2
```

## Ограничения

- IOMMU работает с патчем per-device ops. Без патча NPU считает в non-IOMMU режиме
  (по физическим адресам), для headless-трекера этого достаточно.
- `reg` у NPU (64 КБ с `fde40000`) накрывает окно MMU на `fde4b000`, поэтому драйвер
  пишет `error -EBUSY: can't request region for resource [mem 0xfde40000-0xfde4ffff]`.
  На инференс не влияет; сужение `reg` в оверлее убирает сообщение.
- Частоты выше 800 МГц не проверялись (в OPP только до 800).
- Проверено на RK3566 (Zero 3W). RK3568/RK3588 — по аналогии, но не проверялось.
