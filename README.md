# Rakkashin Kernel

Custom GKI kernel for **avr / avr_retcn** (Android 16, `android16-6.12`, 6.12.58)
with KernelSU-Next and a set of performance, efficiency and battery features,
controlled from a KernelSU WebUI.

by **Rakkashin**

> 日本語の説明は[下](#日本語)にあります。

## Features

**Root**
- KernelSU-Next v3.4.1 (no SUSFS: it broke Chrome and every WebUI on this device)

**Profiles** — `/sys/kernel/avr_profile/mode`, switched from the WebUI

| | CPU frequency | PELT | Launch boost | Thread boost | Sched slice | Writeback | Background apps |
|---|---|---|---|---|---|---|---|
| battery | Energy Model efficiency knee; demand-matched when not touched | 32 ms | gentle (62%) | off | 2× | 30 s | heavily limited |
| smart balanced | demand-matched (≤80%) while not touched, 100% on touch | 16 ms | full | off | stock | 15 s | capped at 50% |
| performance | floor 30% while touched | 8 ms | full | +15% | 0.7× | 15 s | stock |
| game | floor 50% | 8 ms | full | +25% | 0.7× | 15 s | limited |
| extreme | pinned at max; memory bus and GPU at top speed | 8 ms | full | +50% | stock | 15 s | 5% |

- **Extreme**: benchmark-grade. CPUs pinned at max frequency (no ramp-up
  between runs), memory/cache bus (bus_dcvs) and GPU (kgsl) held at top
  speed, foreground apps guaranteed 60% capacity, background 5%. Much more
  heat and drain; thermal limits still apply. Leaving Extreme restores the
  stock bus and GPU settings, so no other profile is affected.
- **Smart Balanced** (default): while nobody touches the screen the CPUs are
  capped at 80% (the least efficient top of the frequency range); an
  in-kernel touch handler lifts the cap the instant a finger lands, and app
  launches lift it too. Full speed while you interact, efficient otherwise.
  Works without the control module.
- **Demand-matched frequency**: while the screen isn't touched, Battery and
  Smart Balanced cap each cluster at the frequency its busiest CPU actually
  needs (measured utilization + 25%), re-evaluated every 20 ms from both
  PELT utilization and the busy time of the last tick. Rises at once, jumps
  by half when a CPU saturates at the cap, falls only after 100 ms of lower
  load. The vendor governor can no
  longer overshoot, so the same work runs at lower frequency and voltage.
- **Sustained performance** (WebUI toggle, off by default): thermal-aware.
  While the phone is cool, touch means full speed as usual. Once the skin
  temperature (front/back virtual sensors) reaches the start temperature
  (default 40 °C, WebUI), the demand-matched CPU and GPU caps stay on while
  the screen is touched too, with headroom shrinking from 70% to 30% over the
  next 5 °C. Touched caps start from the top, jump back to the top as soon as
  a CPU is busy for a whole 20 ms tick at the cap (or the GPU is saturated),
  and only come down after a second of lower load, so frame bursts are not
  squeezed. The phone runs only as fast as the work needs exactly when heat
  builds up, so long sessions throttle less. Combine with per-app profiles
  to use it for chosen games only.
- **GPU efficiency**: the same idea for the GPU. `rk_gpud` (control module,
  `rakkashin/control-module/src/rk_gpud.c`) caps the GPU at the lowest kgsl
  frequency step that covers its measured load + 25% while the demand cap is
  live, rising at once and jumping halfway to the top when saturated (to the
  top while touched). kgsl refreshes its busy figure about once a second, so
  rk_gpud decides once per fresh reading against the clock averaged over the
  same time. It sleeps in poll() on `/sys/kernel/avr_profile/demand_active`
  (sysfs_notify; 0 off, 1 untouched, 2 touched) while inactive, so a touch
  lifts the GPU cap instantly.
- **Power limit**: keep battery drain under a set wattage (WebUI). CPU power
  is predicted every 20 ms from the Energy Model and the cap set to fit the
  budget left after non-CPU power, which a real battery reading calibrates
  each second.
- **Per-app profiles**: give any app its own profile; it applies while that app
  is in the foreground (decided in-kernel on cpuset attach, no polling).
- **Thread boost**: the heaviest foreground threads get a uclamp floor of
  their load plus headroom, so frequency is already up when frame work spikes.
- **App launch boost**: a freshly started app runs flat out until its launch
  work settles (0.3–2 s, adaptive).
- **Sched slice**: EEVDF base slice relative to stock; longer in battery
  (fewer context switches), shorter in performance/game (snappier).
- **Battery caps** come from the Energy Model: the highest frequency whose
  energy cost is within 25% of the most efficient state (50–85% of max).
- CPU limits use cpufreq frequency QoS, so thermal limits always win.

**Memory**
- Keep more apps in RAM: lmkd tuned for 16GB, up to 64 cached apps
- Two-stage zram: lz4 for speed (zram built in with an LZ4 backend; the stock
  module only has lzo/zstd), idle pages re-packed with zstd every 30 min.
  Every zram device is created with zstd as its recompression algorithm, so
  this works on a live swap device and alongside other zram tools.
- Earlier kswapd (`watermark_scale_factor` 50), no watermark boost,
  no swap readahead on zram, `vfs_cache_pressure` 50

**Storage / I/O**
- ADIOS I/O scheduler on the UFS LUNs (by Masahito Suzuki)
- LZ4 1.9.x fast decompression loop: faster reads from the EROFS system partitions
- f2fs: fast-path fsync for files with up to 20 dirty blocks

**Battery / latency**
- Boeffla wakelock blocker (RCU rewrite), with a wakelock list in the WebUI
- vmstat every 20 s, I/O stats and binder logging off, no fs sync on suspend,
  15 s writeback, power-efficient workqueues
- TCP BBR with fq_codel as the default qdisc, no slow start after idle,
  `tcp_notsent_lowat` 128K
- Tuned for the Oryon cores (`-mtune=oryon-1`)

**Compatibility**
- KMI-safe: export CRCs match the stock GKI, and the stock GKI module
  signing certificate is trusted, so stock vendor and system_dlkm modules load.
- `uname -r` reports the stock release string.

Ported ideas from AGNi, NGK, Melt, LawRun and RealKing; original work for the profiles,
per-app switching, thread boost, launch boost and efficiency caps.

## Install

The AnyKernel3 zip ships arm64 `busybox`/`magiskboot` (from Magisk v30.7):
this SoC can't run the 32-bit tools stock AnyKernel3 includes, which broke
flashing with Kernel Flasher.

1. Uninstall any older control module (`avr Profile Switcher`).
2. Flash `Rakkashin-Kernel-*-avr-AnyKernel3.zip` from the KernelSU-Next app
   (or `fastboot flash boot Rakkashin-Kernel-*-avr-boot.img`; the boot image
   is built for firmware W3WWL36.65-11-18 only).
3. Install `Rakkashin-Kernel-Control-*.zip` as a module and reboot.
4. Open the module's WebUI.

Keep your stock `boot.img` to roll back with fastboot.

## Diagnostics

`rk_diag.sh` (in the control module, `/data/adb/modules/rakkashin/rk_diag.sh`)
collects the kernel/module state and samples CPU and GPU caps for 10 s into
`/sdcard/RakkashinKernel/diag.txt`. Run it from Termux with root, leave the
screen untouched while it samples. Module scripts also log to
`/data/adb/rakkashin/rakkashin.log`.

## Build

```sh
. rakkashin/build-env.sh /path/to/out
make -C $KSRC O=$KOUT gki_defconfig   # then merge arch/arm64/configs/avr_custom.fragment
make -C $KSRC O=$KOUT -j$(nproc) \
     CLANG_AUTOFDO_PROFILE=$KSRC/gki/aarch64/afdo/kernel.afdo Image
```

`KBUILD_GENDWARFKSYMS_STABLE=1` (set by `build-env.sh`) is required: without it
symbol CRCs differ from the stock GKI and the phone will not boot. LTO must stay
off (`GENDWARFKSYMS` depends on `!LTO`). `rakkashin/repack_boot.py` swaps the
kernel into a stock boot image and refreshes its AVB hash footer.

Sources: `rakkashin/control-module/` (WebUI module),
`kernel/avr_profile.c`, `kernel/rk_tboost.c`, `kernel/launch_boost.c`,
`drivers/base/power/boeffla_wl_blocker.c`.

## License

GPL-2.0, like the Linux kernel. See `COPYING`.

---

## 日本語

avr / avr_retcn 向けのカスタム GKI カーネルです（Android 16、6.12.58）。
KernelSU-Next と、性能・効率・電池持ちのための独自機能を入れています。
設定は KernelSU の WebUI から行います。

- **プロファイル**: バッテリー / バランス / パフォーマンス / ゲームを切り替えます。
- **アプリごとのプロファイル**: 設定したアプリが前面にある間だけ、そのプロファイルになります。
- **持続パフォーマンス**: 本体が温まってきたら、操作中も CPU と GPU を必要な分だけに抑えて、発熱による性能低下を防ぎます。涼しいうちは全開です。
- **スレッドブースト**: 前面アプリの重いスレッドに、先回りで性能の余裕を与えます。
- **起動ブースト**: アプリの起動直後の1秒だけ、CPU を全開にします。
- **zram 2段圧縮**: 速い lzo-rle と、詰め込みの zstd を組み合わせます。
- **wakelock ブロック**: WebUI の一覧から選べます。

**インストール**: 上の Install の手順と同じです。純正の `boot.img` は必ず保存しておいてください。

作者: **Rakkashin**
