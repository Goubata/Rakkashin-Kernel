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
| battery | capped at the Energy Model efficiency knee | 32 ms | gentle (62%) | off | 2× | 30 s | heavily limited |
| balanced | stock | 16 ms | full | off | stock | 15 s | capped at 50% |
| performance | floor 30% | 8 ms | full | +15% | 0.7× | 15 s | stock |
| game | floor 50% | 8 ms | full | +25% | 0.7× | 15 s | limited |

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
  module only has lzo/zstd), idle pages re-packed with zstd every 30 min
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

1. Uninstall any older control module (`avr Profile Switcher`).
2. Flash `Rakkashin-Kernel-*-avr-AnyKernel3.zip` from the KernelSU-Next app
   (or `fastboot flash boot Rakkashin-Kernel-*-avr-boot.img`; the boot image
   is built for firmware W3WWL36.65-11-18 only).
3. Install `Rakkashin-Kernel-Control-*.zip` as a module and reboot.
4. Open the module's WebUI.

Keep your stock `boot.img` to roll back with fastboot.

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
- **スレッドブースト**: 前面アプリの重いスレッドに、先回りで性能の余裕を与えます。
- **起動ブースト**: アプリの起動直後の1秒だけ、CPU を全開にします。
- **zram 2段圧縮**: 速い lzo-rle と、詰め込みの zstd を組み合わせます。
- **wakelock ブロック**: WebUI の一覧から選べます。

**インストール**: 上の Install の手順と同じです。純正の `boot.img` は必ず保存しておいてください。

作者: **Rakkashin**
