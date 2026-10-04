# Changelog (open-tx-isp)

Condensed from the open-stack campaign changelog; only open-tx-isp (kernel driver) changes.
Newest first, grouped by date. Everything listed was device-tested on the SoC named unless
marked otherwise. Release tags `vYYYY.MM.DD` are planned; until then dates are the reference.

## 2026-10-04

- T20/T10: the unused 8 MiB V4L2-MMAP frame pool is off by default (`isp_mmap_pool_kb=0`, parameter kept): about +8 MB free RAM (T20 MemFree 46.7 to 55 MB, T10 1.9 to ~9.7 MB); only the recovered firmware unit stays `-O0` (-140 KB).
- Shortfall logging: when reserved memory is too small the driver logs once with have/need/missing and a concrete value (`isp_mmap_pool_kb`, `ispmem`).
- Hardening: T31 proc writes bounded, VIC stop busy-wait bounded (~10 ms), AE handle wait with 2 s timeout; T20/T10 IRQ registered before its data was published (oops on early interrupt) fixed.
- T20 AE after a streamer restart: sensor exposure cache invalidated on every sensor sync and AE state reset like on a fresh load (5/5 restarts converge).
- Sensor module pin fixed on T21/T31/T41 (the sensor driver was registered with the ISP module as owner): `rmmod` of a sensor during streaming is refused, no more use-after-free.
- T23: white-balance jump to ~10000 K with the vendor AE fixed (black level rewritten after stream start).
- T41: unload, flip, BCSH and isp-m0 fixes merged into `next`; module 80 KB smaller; three clean reloads.
- `next` branch created; it tracks the tested aggregate (fast-forward only).

## 2026-10-03

- Module size: T23 1,047 to 622 KB (vendor 857), T31 859 to 711 KB (vendor 829), T21 760 to 452 KB (vendor 616), T20 775 to 736 KB, T10 770 to 731 KB.
- T41: module reload root cause fixed (a decompiled tuning-node helper overwrote `.bss`, 10/10 reload cycles clean); picture controls, `isp-m0` in vendor layout, flip registers, sensor re-registration.
- T23: dynamic ADR and defog lifted from the vendor module (44/44 emulator cases identical); lifted vendor AE is the default; review fixes for unload use-after-free, defog allocations, front-crop overflow, ADR/AE locking; front crop on the vendor path.
- T21: `ae_it_max_us` acts (beyond vendor, kept by decision); AWB hysteresis at dusk (39 switches to 0 in 40 frames); colour-temperature updates only above 50 K change; lifted AWB 1.41x to 0.95x of vendor instructions.
- T10/T20: `ae_it_max_us` limits the AE; Sinter/Temper strength acts; CCM/LSC readable in `isp-m0`; T10 reload safe (5 cycles, build for the wrong kernel tree refused); T10 duplicate `isp_printf` export fixed.
- T10: sub-stream and diagonal drift fixed in the lifted firmware shared with T20.
- Hardening: QBUF buffers must lie in the rmem window (`qbuf_guard=0` disables); sensor module pinned while the ISP is open; AVPU review fixes (minor-number leak, use-after-free on unbind, flush range).
- T31: privacy mask implemented like the vendor (400/400 random sequences register-identical); scene mode and colour effects on T23/T31; tuning stubs replaced (black level, colour matrix presets, front crop, scaler level).
- T30 readiness without hardware: builds for a real T30 kernel, missing `isp_printf` export fixed.
- Kernel patches 0099/0100/0101 for the soc_vpu driver and rmem flush ioctl (sleeping wait, immediate error interrupt, validated flush range, statistics registers readable for the vendor T20 rate controller).

## 2026-10-02

- Robustness: T23 10 stop/start and reload cycles without an oops, two out-of-bounds writes fixed, `.bss` 434 to 180 KB; T20 `rmmod` while streaming refused, 53 user copies checked; T31 vmalloc leak (252 KB per cycle) fixed; T21 reload oops root cause (statistics DMA into freed buffers) fixed.
- Independent review: deadlock between sensor unload and `/proc/jz/sensor/*`, orphan sensor slots, T21 open/release counting.
- T21: white balance, CT detection and CT-driven CCM/LSC lifted from the vendor module (10/10 scenes register-identical); event callbacks with IRQs off like the vendor.
- T23: gain index for gain-driven blocks was linear instead of log2; sharpen parameters loaded from the active IQ bank; exposure kept across stream restarts; failed stream start and reload fixed (statistics DMA freed while active); vendor AE chain behind `source_ae_oem`.
- T20: vertical flip chroma overrun (pink band) and sharpness fixed; vendor AWB chain behind `t20_simple_awb=0` after fixing decompilation errors.
- T10 integrated: the NVPU wrote 21 KB/10 KB past each reference plane, fixed; day/night panic from a zeroed FSM manager pointer fixed.

## 2026-09-30 to 2026-10-01 (campaign start)

- T31: lifecycle hardening (locking around frame-channel ioctls, STREAMOFF races, last-close use-after-free), `O_NONBLOCK` DQBUF, tuning gaps closed (RGB coefficients, AE ROI, SensorAttr, isp-w02 VIC counters, per-frame WaitFrame).
- T23: live exposure readback and vendor-format `isp-m0`; day-to-night oops fixed; ~45 control IDs that silently returned success are wired or rejected; night purple fixed; module 1,598 to 1,095 KB.
- T20: vendor-format `isp-m0`, correct AE exposure readback, oops on timps stop/restart fixed (20 clean cycles), max analog gain applied by the AE.
- T21 (first open bring-up): exposure readback, unreachable controls, night flicker (stock AE lifted instruction by instruction), noise, night mono, colour blotches, overexposure, control dispatchers lifted from the vendor module.
