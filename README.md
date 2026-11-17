# ROLEXWIN

**Rolex Oyster Perpetual Datejust** — high-fidelity Windows desktop clock.

A native Win32/Win64 application that renders a realistic Rolex Datejust with authentic baton hands, cyclops date magnification, and a mechanical tick-tock sound at **28,800 bph**.

<img width="500" height="974" alt="image" src="https://github.com/user-attachments/assets/3e418c16-0401-4dbd-ad76-696a668d84e9" />


## Features

- Authentic **baton hands** with Chromalight-style lume (metal tip → lume channel → metal base)
- **Cyclops lens** (2.5× magnification) over the date window
- Clean **vector date digits**
- Procedural **mechanical tick-tock** at real automatic frequency (28,800 alternances/hour)
- Fullscreen support (`F11` / Maximize)
- Fully self-contained (dial image and icons are embedded)
- Smooth real-time second hand

## Build

### Visual Studio / MSVC

```bat
cl /EHsc /O2 /DUNICODE /D_UNICODE /TP ROLEXWIN.CPP /link gdiplus.lib user32.lib gdi32.lib ole32.lib winmm.lib /SUBSYSTEM:WINDOWS
```

### MinGW / g++

```bash
g++ -O2 -municode -mwindows ROLEXWIN.CPP -o ROLEXWIN.EXE -lgdiplus -luser32 -lgdi32 -lole32 -lwinmm
```

Required files in the same directory:

| File            | Description                                      |
|-----------------|--------------------------------------------------|
| `ROLEXWIN.CPP`  | Application logic                                |
| `ROLEXWIN.H`    | Geometry + embedded dial and icons               |

A Visual Studio project (`.vcxproj`) is also provided for convenience.

## Requirements

- Windows (x86 or x64)
- GDI+
- No external assets needed (everything is embedded)

## Controls

| Key / Action        | Effect                    |
|---------------------|---------------------------|
| `F11`               | Toggle fullscreen         |
| Maximize button     | Enter fullscreen          |
| `Esc`               | Exit fullscreen / Quit    |

## Technical notes

- Hands are drawn with multi-layer polygons (dark outline, steel facets, central lume insert).
- The date is rendered into a clean off-screen buffer and sampled by the cyclops using high-quality bicubic interpolation.
- The tick sound is procedurally generated (short metallic click + FM body) and played from memory via `PlaySound`.
- The second hand uses 1/256-circle resolution for smooth motion.

## Related projects

This is part of a series of Rolex-themed clocks by the same author:

| Project | Description | Link |
|---------|-------------|------|
| **ROLEX386** | Original DOS version (VGA + AdLib / OPL2) | [github.com/javiergutierrezchamorro/ROLEX386](https://github.com/javiergutierrezchamorro/ROLEX386) |
| **ROLEX.COM** | Classic DOS `.COM` implementation | [github.com/javiergutierrezchamorro/ROLEX.COM](https://github.com/javiergutierrezchamorro/ROLEX.COM) |
| **ROLEZX** | Another variant in the series | [github.com/javiergutierrezchamorro/ROLEZX](https://github.com/javiergutierrezchamorro/ROLEZX) |
| **ROLEXWIN** | This project — modern Win32/Win64 port with GDI+ | — |

## License

This is a fan-made visualization for educational and personal use.

Rolex, Datejust, Oyster Perpetual and Chromalight are trademarks of Rolex SA.  
This project is **not** affiliated with, endorsed by, or connected to Rolex SA.
