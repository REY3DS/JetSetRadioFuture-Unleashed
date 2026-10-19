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
| Runtime platform layer (`src/platform`) | **needs a Switch implementation** |
| Guest memory (4 GB reservations, mirrors, apertures) | **needs a Switch translation** |
| First `.nro` on hardware | not yet |

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
sdmc:/switch/JSRFUnleashed/save/       engine cache and disk images
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
