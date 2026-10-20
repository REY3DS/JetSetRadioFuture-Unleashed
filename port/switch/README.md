# Nintendo Switch port

The engine on a Nintendo Switch: devkitA64 / libnx, SDL2 for the window, sound
and pads, and OpenGL ES 3.2 through the mesa port that devkitPro ships. This is
the same `NV2A_GL_CONTEXT=sdl` + `NV2A_GLES` path the Android build uses.

## Status

| Piece | State |
|---|---|
| Cross build (CMake + devkitA64) | configures; `nv2a_gl context: sdl, OpenGL ES 3.2` |
| `port/switch/jsrf_switch.c` (SD card folders, log, engine thread) | written |
| `main.c` Switch entry | wired |
| Runtime platform layer (`src/platform`) | written: `win32_compat.c` has Switch branches for `VirtualAlloc`/`Protect`, file mappings, `pread`/`pwrite`, `timegm`, `memalign`, `sysinfo`, `backtrace` |
| Guest memory (4 GB reservations, mirrors, apertures) | translated in `xbox_memory_layout.c`: linear RAM plus a window table (contig, kernel page, NV2A, MCPX, flash) behind `xbox_HostPtr` |
| Cross build | compiles and links; `jsrf_recomp.nro` produced (53 MB) |
| First `.nro` on hardware | runs: the engine mounts the game folder, finds `TDATA`/`UDATA`, reports the cache partition's free space and reaches the title's interrupt setup. That first log stopped mid-line because the card had been filled with preallocated disk images -- see *Disk images and the SD card* |
| First crash | vblank thread, first frame after `KeConnectInterrupt`: the kernel bridge translated an NV2A register address by addition -- see *Address translation in the kernel bridge* |

The two open items are the real work: see *Why the platform layer is different*
below.

## Toolchain

A normal devkitPro install works:

```
dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-glad
cmake -S port -B build-switch \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-switch -j
```

The generated code (`port/recomp/gen`) is the same one the Linux build uses;
it is produced from the player's own `default.xbe` by `tools/recomp`.

If `pkg.devkitpro.org` is unreachable (Cloudflare refuses some networks), the
toolchain can be taken from the official container image without a container
daemon: pull `devkitpro/devkita64`'s layers from `registry-1.docker.io`,
extract `/opt/devkitpro`, and run the toolchain against the image's Debian 12
glibc with `proot` (the host's glibc may be older than the toolchain's). That is
how this tree was first configured; it changes nothing in the repository.

## SD card layout

```
sdmc:/switch/JSRFUnleashed/default.xbe
sdmc:/switch/JSRFUnleashed/Media/...
sdmc:/switch/JSRFUnleashed/save/       engine cache and disk images (a few MB; see below)
sdmc:/switch/JSRFUnleashed/settings    key=value, as on every host
sdmc:/switch/JSRFUnleashed/Logs/       last-run.log, previous-run.log
```

The game's own saves stay in the game folder under `UDATA`, as on hardware.
Output goes to `nxlink` when the `.nro` was started by it (`nxlink -s`), and to
`Logs/last-run.log` otherwise.

## Why the platform layer is different

On Linux and macOS the runtime reserves 4 GB of address space, maps the guest's
64 MB of RAM, and then maps the *same* pages again every 64 MB -- the Xbox
memory controller's 26-bit bus wraps RAM, so guest address `0x20000448` really
is `0x00000448`. It also places the NV2A (`0xFD000000`), MCPX (`0xFE800000`),
flash and tiled apertures at their exact guest addresses. Guest addresses are
then host addresses plus one offset, which is what `XBOX_PTR` is.

libnx exposes no `mmap`, `mprotect` or address reservation: a Switch process
cannot ask for a 4 GB span or alias physical pages. The port therefore needs a
software translation instead of a pointer offset:

- guest RAM in one aligned allocation, with mirrors handled by masking the
  address (RAM size is a power of two);
- the NV2A/MCPX/flash/tiled apertures as separate allocations, selected by
  range;
- `VirtualAlloc` / `VirtualProtect` / file mappings / `VirtualQuery` emulated
  over `malloc` and a small registry (no page protection on Switch);
- suspend/resume, `SIGUSR2`, `memfd_create`, `sysinfo` and `backtrace` replaced
  (none exist in devkitA64's newlib).

Nothing here needs executable guest memory: the recompiled title is native ARM64
code in the `.nro`, and the guest's `.text` is only data.

## Address translation in the kernel bridge

`XBOX_PTR` is not the only place that turns a guest address into a pointer: the
kernel bridge does the same arithmetic by hand (`BRIDGE_MEM32`, `BRIDGE_MEM16`,
`BRIDGE_MEM8`, `XBOX_TO_NATIVE`), and so do the APU register shim in
`port/main.c`, the hand-lifted PGRAPH handler in `port/recomp_manual.c`, the
zeroing in `MmAllocateContiguousMemoryEx` and OHCI's descriptor reads. That is
correct on a host that really maps the console's address space, and true only
below the RAM size here.

The first frame after `KeConnectInterrupt` made it visible: raising the vblank
goes through `BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0)`, which read
"RAM base + `0xFD600100`" -- four gigabytes past a 64 MB allocation, a
translation fault in the vblank thread on a timer, while the game itself carried
on for two more allocations. The AC'97 bus master came next (a read of
`0xFEC0012C` through the APU shim), and D3D's PGRAPH handler and OHCI's
descriptors were waiting behind it. Those call sites now go through the same window
table as everything else (`BRIDGE_NATIVE_PTR` in `kernel_bridge.c`, with the
addition kept inline below the RAM size), and `xbox_GuestRangeMapped()` answers
"is there memory behind this guest range" so a bounds check written to reject a
bad out-pointer does not reject the contiguous block.

Permission bits are meaningless on this host: the freed page at address zero is
not mapped at all, so a null write is a hardware fault before any check runs --
which is why those checks are about *ranges*, not protection.

## Disk images and the SD card

JSRF reaches the console's disk through `\Device\Harddisk0\partitionN`. On the
hosts those are sparse image files under `save/`: the whole disk (`Partition0`,
5 GB) carries the retail partition table at sector 4, and the title's cache
partition (`Partition5`, 750 MB) stands in for the utility drive. Partitions
1..4 exist only so the geometry looks retail; they are never opened as devices.

Measured on the host, the title writes **one sector** of `Partition0` and about
**116 KB** of `Partition5`. The rest of those files is a hole.

An SD card is exFAT through FatFS, which has no sparse files. Preallocating the
retail geometry therefore wrote 4.5 GB on every launch; the 5 GB disk image did
not fit (and cannot, on a FAT32 card); and once the card was full the *log*
began losing writes, so the run ended with a log that stopped in the middle of a
line -- which reads like a crash but is a full card.

The Switch build creates the images empty and lets them grow only as far as the
title writes. `xbox_partition_image_size()` (`kernel_path.c`) answers
`NtQueryInformationFile` with the retail size, so the guest still sees a 5 GB
disk and a 750 MB cache partition, and reads past the end of a device handle
come back as zeroes, which is what an unwritten sector looks like.

A card that ran an older build has ~4.5 GB of images in `save/`: delete `save/`
once (or just `Partition*.img`). The engine recreates what it needs, and the
game's own saves live in `UDATA`, not here.
