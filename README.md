# Xbox 360 Homebrew — XboxHello + Plex360

Two native `.xex` apps for **RGH/JTAG** consoles (or devkit), with a
standalone command-line toolchain:

- **Plex360** — native Plex client: library browsing, poster grid, detail
  pages, photo viewer, and video playback (local WMV + streamed transcode).
  BSD sockets + XML + JPEG + XMedia2/XAudio2.
- **XboxHello** — D3D9/XInput demo (advanced Hello World).

## Build — nothing to install

The XDK 21256.3 toolchain (Xenon PPC compiler, linker, imagexex) is used
directly from the command line. **VS2010 is NOT required.**

```
build.bat            → build\Release\bin\Plex360.xex
build.bat XboxHello  → build\Release\bin\XboxHello.xex
```

## Plex360 — configuration

Copy `build\Release\bin\Plex360.xex` **and** `config.ini` to the console
(e.g. `Hdd1:\Homebrew\Plex360\`). Edit `config.ini`:

```ini
server=192.168.1.10   ; your Plex server IP
port=32400
;token=               ; optional if LAN is "allowed without auth"
```

Token tip: Plex Web → Settings → Network → *"List of IP addresses and
networks that are allowed without auth"* → add `192.168.1.0/24` (adjust to
your subnet) → no token needed.

## Plex360 — controls

| Input            | Action                                       |
|------------------|----------------------------------------------|
| D-PAD            | Navigate sections / poster grid              |
| A                | Open section/item, fullscreen photo, play    |
| B                | Back                                         |
| LB / RB          | Page jumps in the grid                       |
| X (libraries)    | Play local `test.wmv`                        |
| Y (libraries)    | Play `test.wmv` via USER_IO stream           |
| BACK + START     | Quit (back to dashboard)                     |

Screens: libraries → poster grid (background-loaded via
`/photo/:/transcode`) → detail (synopsis, duration, year) → photo viewer.

## Video playback

`IXMedia2XmvPlayer` (xmedia2) plays WMV/ASF: **WMV3 / VC-1 + WMA** codecs
(WMV2 is rejected). Sources that aren't already WMV go through
`pctest\PlexRelay` — a tiny TCP HTTP server on the PC (port 8090, no admin
rights needed) that transcodes with Windows `MediaTranscoder` and serves
the growing `.wmv` by byte ranges to the console's USER_IO stream.

```
PlexRelay.exe      → http://<pc>:8090  (/start /read /status /stop)
```

## XboxHello — controls

| Input            | Action                          |
|------------------|---------------------------------|
| Left stick       | Move the cursor                 |
| A                | Rumble + press counter          |
| BACK + START     | Quit (back to dashboard)        |

## Deploying to the console (RGH/JTAG)

- **USB**: copy the `.xex` to a FAT32 stick → run it from
  Aurora / FreestyleDash / XeXMenu (file browser → launch the .xex).
- **LAN**: FTP into the running dashboard (user `xboxftp`, port 21) —
  `pctest\deploy.py` does it — or Xbox 360 Neighborhood with a full
  VS2010+SDK setup.

## Testing without a console

**Xenia Canary** runs homebrew `.xex` files: drag `Plex360.xex` onto
`xenia_canary.exe` (networking is hit-or-miss).

## Layout

```
XboxHello.sln              VS2010 solution (optional, if VS2010+XDK installed)
build.bat                  Generic standalone build: build.bat [Project]
XboxHello/
  XboxHello.vcxproj        "Xbox 360" platform project (toolset 2010-01)
  src/
    main.cpp               App: D3D9 + shaders + XInput + HUD (~450 lines)
    font8x8.h              Embedded 8x8 bitmap font (public domain)
Plex360/
  Plex360.vcxproj          Xbox 360 project
  config.ini.example       Server config template
  src/
    main.cpp               Screen machine + worker thread + net queue + XMV
    net.{h,cpp}            HTTP GET over BSD sockets (XNetStartup, DNS, chunked)
    plex.{h,cpp}           Plex API paths + XML response parsing
    xmlmini.{h,cpp}        XML element extractor (tags + attributes)
    renderer.{h,cpp}       D3D9 quads/text/images (3 batches, single VB)
    font8x8.h              Bitmap font
pctest/
  PlexRelay/               C# relay: transcode → WMV, byte-range reads
  deploy.py                FTP deploy to the console
  fakeplex.py              Fake Plex server for testing
```

## Technical notes

- **No fixed-function pipeline** on Xenon: everything goes through SM3
  shaders compiled at runtime via `D3DXCompileShader`.
- `D3DRS_HALFPIXELOFFSET` enabled for pixel-aligned 2D quads.
- `D3DUSAGE_DYNAMIC` / `D3DLOCK_DISCARD` don't exist on Xenon.
- CPU-written textures must use `D3DFMT_LIN_*` formats (Xenon swizzles
  regular textures in UMA memory).
- **Retail sockets are encrypted**: `XNET_STARTUP_BYPASS_SECURITY` is
  ignored on retail consoles — each `socket()` must be switched to
  plaintext with the undocumented `setsockopt` options `0x5801`/`0x5802`
  (`Net::MakeInsecure`), or LAN `connect()` times out.
- Plex360: network results arrive on a queue (worker thread → main);
  each list request carries a ticket to discard stale responses.
- USER_IO streaming: pass `XMEDIA_CREATE_SHARE_IO_CACHE` plus the *same*
  callback and context for audio+video so the player demuxes one shared
  ASF stream itself.
