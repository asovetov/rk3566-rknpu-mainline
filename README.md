# RK3566 NPU on mainline 6.18 (Armbian current-rockchip64)

Working recipe: the Rockchip NPU (RKNPU2) runs on a **vanilla 6.18.44 kernel**, with
no vendor BSP. Verified on a Radxa ZERO 3W (RK3566).

## Results

`resnet18` (224×224×3 → 1000), 30 runs on the board:

| governor | NPU clock | per frame | FPS | NPU load |
|---|---|---|---|---|
| `performance` | 800 MHz | **9.08 ms** | **110.2** | 88% |
| `simple_ondemand` | 200 MHz (no time to ramp) | 34.60 ms | 28.9 | 88% |

DVFS really switches: `devfreq` exposes 200/297/400/600/700/800 MHz, `cur_freq`
moves, `debugfs` answers with `RKNPU driver: v0.9.8`.

## Why "just build a newer kernel" does not work

- Mainline has **no NPU driver for RK3566**. The in-tree `accel/rocket` driver
  supports RK3588 only and speaks a different ABI (DRM accel), while
  `librknnrt.so` requires the vendor `/dev/rknpu` ioctls.
- So an out-of-tree module is needed. Base:
  [w568w/rknpu-module](https://github.com/w568w/rknpu-module) — a port of the
  vendor `rknpu` 0.9.8 to mainline, tested on 6.19/7.0.

## Fixes required for 6.18 (all in `patches/0001-6.18-compat.patch`)

1. **`linux/devfreq-governor.h` exists only since 6.19.** Added a compat shim
   (`src/include/compat/linux/devfreq-governor.h`) that pulls `<linux/devfreq.h>`
   on ≤6.18.
2. **`devfreq_add_governor()` is not exported to modules before 6.19.** The custom
   `rknpu_ondemand` governor is kept for 6.19+ only; on 6.18 the in-tree
   `simple_ondemand` is used.
3. **`df->data` for `simple_ondemand`.** The driver passed a `rknpu_device`
   pointer, but the governor casts `df->data` to
   `struct devfreq_simple_ondemand_data` — garbage in
   `upthreshold`/`downdifferential`, and every 50 ms the kernel logs
   `devfreq ...: dvfs failed with (-22) error` (hundreds of lines, floods the
   console). Pass `NULL` on 6.18.
4. **No devfreq frequency window was set.** Without it `simple_ondemand` asks for
   0 Hz, `devfreq_recommended_opp()` returns `-EINVAL`, same spam. The window is
   now taken from the OPP table (`devfreq range: 200000000-800000000 Hz`).
   Note the 6.18 field names: `scaling_min_freq` / `scaling_max_freq`.
5. **`vermagic`.** Building against copied kernel headers lets `make prepare`
   regenerate `utsrelease.h` without the local version, so the module claims
   `6.18.44` instead of `6.18.44-current-rockchip64` and refuses to load. Fix
   `include/generated/utsrelease.h` in the header tree and rebuild `rknpu.mod.o`.

The device-tree overlay needed board-specific changes too
(`dts/rk3566-rknpu-overlay.dts`):

- `SCMI_CLK_NPU` for RK356x is **2** (6.18 headers only carry the RK3588 value
  `6`; the vendor `rk356x.dtsi` uses `<&scmi_clk 2>`);
- on this board the NPU rail is `vdd_gpu_npu` (RK809 DCDC_REG2, shared with the
  GPU), not `vdd_npu`;
- `interrupt-parent = <&gic>` added to the NPU and IOMMU nodes;
- IOMMU left `disabled` (non-IOMMU mode): `rockchip-iommu` v2 does not allocate
  page tables below 4 GB — a known driver bug, not fixable from a module.

## Build

You need the target kernel headers and an aarch64 cross compiler.

```bash
# headers: copy from the board (or the image). They were built on aarch64, so the
# host tools must be rebuilt for x86 first:
cp -a /mnt/armbian/usr/src/linux-headers-$(uname -r) /tmp/kheaders
cd /tmp/kheaders
python3 -c "import pathlib;[p.unlink() for p in pathlib.Path('scripts').rglob('*.o')]"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- HOSTCC=gcc HOSTLD=ld prepare scripts

# modpost in the header tree is an arm64 binary as well — rebuild it by hand:
cd scripts/mod && gcc -O2 -Wall -I. -I../../tools/include -I../../scripts/include \
    -o modpost modpost.c file2alias.c sumversion.c symsearch.c

# module
cd rknpu-module && make KDIR=/tmp/kheaders ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# check: modinfo rknpu.ko | grep vermagic  →  6.18.44-current-rockchip64 ...

# overlay
make dtbo KDIR=/tmp/kheaders
fdtoverlay -i /boot/dtb/rockchip/rk3566-radxa-zero-3w.dtb -o /tmp/check.dtb dts/rk3566-rknpu.dtbo
```

## Install on the board

```bash
install -m644 rknpu.ko /lib/modules/$(uname -r)/updates/ && depmod -a
mkdir -p /boot/overlay-user && cp rk3566-rknpu.dtbo /boot/overlay-user/
echo 'user_overlays=rk3566-rknpu' >> /boot/armbianEnv.txt
reboot
```

Verify:

```bash
dmesg | grep -i rknpu
# RKNPU: rknpu iommu device-tree entry not found!, using non-iommu mode
# [drm] Initialized rknpu 0.9.8 for fde40000.npu on minor 1
# devfreq range: 200000000-800000000 Hz
ls /dev/dri/by-path/ | grep npu      # platform-fde40000.npu-render
cat /sys/kernel/debug/rknpu/version  # RKNPU driver: v0.9.8
```

For maximum throughput (a tracker cares about latency, not power):

```bash
echo performance > /sys/class/devfreq/fde40000.npu/governor
```

With `simple_ondemand` short bursts stay at 200 MHz — the governor does not ramp
fast enough, hence 34.6 ms versus 9.1 ms.

## Inference test without Python on the board

`scripts/rknn_bench.c` is a minimal C benchmark against `librknnrt.so`, cross
compiled with `aarch64-linux-gnu-gcc`; the model is the prebuilt
`resnet18_for_rk3566_rk3568.rknn` from `rknn-toolkit2`.

```bash
LD_LIBRARY_PATH=/usr/lib ./rknn_bench resnet18_for_rk3566_rk3568.rknn 30
# librknnrt: 2.5.0 / driver: 0.9.8
# iters: 30  per-frame: 9.08 ms  fps: 110.2
```

## Limitations

- IOMMU is off, so the NPU works with physical addresses. Fine for a tracker; to
  enable IOMMU you need a `rockchip-iommu` fix (GFP_DMA32) or `mem=3840M` on the
  kernel command line.
- Frequencies above 800 MHz were not tested (the OPP table stops at 800).
- Verified on RK3566 (ZERO 3W). RK3568/RK3588 should be similar but are untested.

## Credits

The out-of-tree port itself is [w568w/rknpu-module](https://github.com/w568w/rknpu-module);
the vendor driver is Rockchip's `rknpu` 0.9.8. This repository only carries the
6.18 compatibility fixes, the Radxa ZERO 3W device tree overlay and the test
harness.
