# DFBHD_Widescreen_Scope_Mod

An unofficial fan mod for **Delta Force: Black Hawk Down** (2003, NovaLogic).
Fixes the widescreen distortion of the sniper scope and lets you fully replace
the in-game scope graphic with a custom image.

> This is an independent fan project, not affiliated with NovaLogic / THQ.
> No original game assets are included. `d3d8.dll` is a Direct3D8 **proxy DLL**
> that forwards all standard calls to the real system library.

---

## What does it fix?

The original game was built for 4:3 displays, so on a 16:9 widescreen monitor
the sniper scope's circular view appears stretched into an oval.

- Fixes the scope's circular field of view (restores a true circle)
- Replaces the game's default scope graphic (circle + reticle) with a
  **custom user-provided TGA image**
- Works correctly with Night Vision (NVG) alone and NVG + scope combined
- Optional patch layer to cover a minor vanilla rendering artifact (a thin
  gray line) at the minimap's edge

## Installation

1. Download `d3d8.dll` from Release page.
2. Place it in your game's installation folder (where `dfbhd.exe` is located,
   usually something like
   `...\Steam\steamapps\common\Delta Force Black Hawk Down 32680\`).
3. (Optional) To use a custom scope image, add the following files to the
   same folder:
   - `scope_overlay.tga` — replaces the entire scope view
   - `minimap_patch.tga` — covers the minimap edge artifact (optional)
4. Launch the game — it applies automatically.

If these image files are missing, the mod silently falls back to default
behavior, so installing the DLL alone is safe even without custom textures.

## Creating custom TGA images

- Resolution: match your actual play resolution (e.g. 1920×1080)
- Format: **32-bit TGA with an alpha channel**
- `scope_overlay.tga`: make the circular lens area alpha = 0 (fully
  transparent), and paint the border/reticle design as opaque however you like.
- `minimap_patch.tga`: make only the area you want hidden opaque; everything
  else fully transparent.

## A note on antivirus false positives

This DLL works as a **proxy DLL** that intercepts the game's Direct3D8 calls
to inject custom rendering — the same general technique used by tools like
ReShade or ENBSeries. It's a legitimate and widely used modding method, but
because it structurally resembles techniques sometimes used by game cheats,
heuristic/ML-based antivirus engines (such as Windows Defender) may
occasionally flag it as a false positive (e.g. `Trojan:Win32/Sabsik.FL.A!ml`).

**You can verify there's nothing malicious here:**
- The file has also been checked through VirusTotal and was not detected by
  any participating security engines at the time of submission.
- The full source code (`d3d8_proxy.cpp`) is published in this repository —
  read it yourself, or build it from source if you'd rather not trust the
  binary (instructions below).
- It makes no network connections, accesses no files outside the game's own
  folder, injects into no other process, and collects no data.
- It only reads texture files from the game's own directory and intercepts
  Direct3D8 rendering calls.

If your antivirus blocks the download:
1. Windows Security → Virus & threat protection → Add an exclusion → select
   your game folder.
2. If the file was already quarantined, restore it from "Protection history."

## Building from source

Built with MinGW-w64 (32-bit target):

```bash
i686-w64-mingw32-g++ -std=c++11 -O2 -shared \
  -static-libgcc -static-libstdc++ \
  -o d3d8.dll d3d8_proxy.cpp d3d8.def \
  -ld3d8 -Wl,--subsystem,windows -Wl,--kill-at
```

## License / Usage

Free to use, modify, and redistribute for personal, non-commercial purposes.
No original game assets are included — please only use this mod if you own
a legitimate copy of the game.

## Credits

Created by Urialia.
README and code cleanup done with Claude (Anthropic).
