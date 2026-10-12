# adam

> [!WARNING]
> **Made with AI.** It has not been reviewed by experienced developers, so expect mistakes.

A static recompilation of **Neon Genesis Evangelion** (新世紀エヴァンゲリオン) for the N64 nto a native Windows program, built with [N64Recomp](https://github.com/N64Recomp/N64Recomp). The game's MIPS code is translated to C ahead of time and
compiled for the PC; graphics are drawn by [RT64](https://github.com/rt64/rt64), and the launcher, settings menus
and controller support come from [RecompFrontend](https://github.com/N64Recomp/RecompFrontend).

This is a hobby and learning project, shared so others can learn from it.

**No game data is included.** Neither this repository nor the download contains a ROM or any game code. You need
your own copy of the game; your ROM is recognised by its hash.

## Download and play

1. Download the zip (`adam-<date>-<commit>.zip`) from the [Releases](../../releases) page and unzip it into a folder
   you can write to. It holds just `adam.exe`, three DLLs, `HOW_TO_PLAY.txt` and `LICENSE.txt`: the menus' fonts and
   icons are built into `adam.exe`, which unpacks them into a `data` folder next to itself on the first start.
2. Run `adam.exe`, choose **Load ROM** and pick your ROM (Japanese release, `.z64`, 32 MB). It is checked and copied
   into the `data` folder next to `adam.exe` (saves and settings go there too), so you only do this once.
3. Choose **Start Game**.

The downloaded `adam.exe` contains no game code: when the game starts, it translates the game's code from **your**
ROM into PC code (N64Recomp's live recompiler, well under a second) and runs the audio microcode with a small
built-in interpreter. In our tests the intro, the menus and the second mission look and run the same as with the
ahead-of-time build described below. `HOW_TO_PLAY.txt` in the zip lists the controls.

Requirements: Windows 10/11 x64, a GPU with Direct3D 12 or Vulkan. Nothing to install (the Visual C++ runtime is
built in).

## Status

- Boots and runs the intro, the menus, the first mission and the start of the second at a steady 30 fps (as on
  hardware), with sound.
- Saves (EEPROM) work. A Project64 `.eep` save copied to the start of `runtime\build\data\saves\adam.bin` loads
  (the other direction is untested).
- Launcher (Load ROM / Start Game, Controls, Settings, Exit), settings menu, remappable keyboard and gamepad input.
  Esc (or the gamepad's Back button) opens the settings in game and pauses the game.
- Fast-forward: hold Tab or the right trigger (2x/4x/8x, set in Settings → General; sound is muted meanwhile).
- Once your ROM is loaded, the launcher shows a picture from the game as its background (read from your ROM).
- Later parts of the game have not been played through yet. Some overlays (code the game loads on demand) have
  never run; expect problems there. Reports are welcome.

## Building from source

### What you need

- Windows 10/11, x64
- Your own ROM of the game (Neon Genesis Evangelion, Japanese release): big-endian `.z64`, 32 MB. XXH3-64 hash `0x43A8E44E8287EB13`
  (`adam.exe --hash <rom.z64>` prints it).
- [Visual Studio](https://visualstudio.microsoft.com/) with "Desktop development with C++" (for CMake, Ninja and the
  Windows SDK). `runtime\build.ps1` expects Visual Studio 18 (2026) Community in its default folder; change `$vsRoot`
  there for another edition.
- [LLVM](https://github.com/llvm/llvm-project/releases) (clang-cl) installed in `C:\Program Files\LLVM`
- [Python 3.12](https://www.python.org/) with `pip install splat64[mips]` (splat and rabbitizer)
- [Git](https://git-scm.com/)

### Folder layout

The dependencies live **next to** this folder (the build scripts and CMake find them there):

```
<any folder>\
  adam\                (this repository)
  N64Recomp\           https://github.com/N64Recomp/N64Recomp         commit ffb39cd
  N64ModernRuntime\    https://github.com/N64Recomp/N64ModernRuntime  commit cdf5abb
  rt64\                https://github.com/rt64/rt64                    commit 8bd2cf1
  RecompFrontend\      https://github.com/N64Recomp/RecompFrontend    commit b1a1477
```

These are the commits adam was built and tested with. `runtime\build.ps1` applies small patches from
`runtime\patches\` to RT64, RecompFrontend and N64ModernRuntime; at other commits they may not apply.

### Building

Run these in PowerShell from the parent folder (`<any folder>`).

1. Get the dependencies at the tested commits:

   ```powershell
   foreach ($d in @(@("N64Recomp","https://github.com/N64Recomp/N64Recomp","ffb39cd"),
                    @("N64ModernRuntime","https://github.com/N64Recomp/N64ModernRuntime","cdf5abb"),
                    @("rt64","https://github.com/rt64/rt64","8bd2cf1"),
                    @("RecompFrontend","https://github.com/N64Recomp/RecompFrontend","b1a1477"))) {
       git clone $d[1] $d[0]
       git -C $d[0] checkout $d[2]
       git -C $d[0] submodule update --init --recursive
   }
   ```

2. Build N64Recomp (it also builds RSPRecomp) and copy `N64Recomp.exe` into `adam\`:

   ```powershell
   cmake -S N64Recomp -B N64Recomp\build
   cmake --build N64Recomp\build --config Release
   Copy-Item N64Recomp\build\Release\N64Recomp.exe adam\
   ```

3. Put your ROM at `adam\rom\game.z64`.

4. Disassemble the ROM and recompile it to C (splat, then N64Recomp; writes `adam\RecompiledFuncs\`):

   ```powershell
   cd adam
   powershell -ExecutionPolicy Bypass -File scripts\rebuild.ps1 *> rebuild_log.txt
   ```

   The last line of `rebuild_log.txt` should be `exit code: 0`.

5. Recompile the game's audio microcode (writes `rsp\aspMain.cpp`):

   ```powershell
   ..\N64Recomp\build\Release\RSPRecomp.exe rsp\aspMain.toml
   ```

6. Build the program (downloads SDL 2.30.3 on the first run):

   ```powershell
   powershell -ExecutionPolicy Bypass -File runtime\build.ps1 *> runtime_build_log.txt
   ```

7. Run `runtime\build\adam.exe`, choose **Load ROM** and pick your ROM. It's copied to `runtime\build\data\`
   (with your saves and settings), so you only do this once.

Everything generated from the ROM (`rom\`, `RecompiledFuncs\`, `rsp\*.cpp`, `splat\asm\`, the exe) is listed in
`.gitignore`. Please keep it that way and don't share those files: the `adam.exe` built this way **contains the
game's code** and must not be shared.

### Building the download (live build)

The shareable build recompiles the game code at startup instead. Steps 1 and 2 above are still needed; steps 4 and 5
are not (it uses `symbols.toml` and `config.toml` from this repository):

```powershell
powershell -ExecutionPolicy Bypass -File runtime\build.ps1 -Live *> runtime_build_live_log.txt
powershell -ExecutionPolicy Bypass -File scripts\package_live.ps1
```

The first command builds `runtime\build-live\adam.exe`; the second copies it with its DLLs, `HOW_TO_PLAY.txt`
and the license into `packages\adam-<date>-<commit>\` plus a zip (no folders: `symbols.toml`, `config.toml` and
`runtime\assets\` are built into the exe), and refuses to include ROMs, saves or debug files.
`runtime\build-live\adam.exe --live-test <rom.z64>` recompiles everything in the console and reports problems.

## Playing

Default controls (change them in Settings → Controls):

| N64 | Keyboard | Xbox-style gamepad |
|---|---|---|
| Stick | arrow keys | left stick |
| A / B | X / C | A / X |
| Z | Z | left trigger |
| Start | Enter | Start |
| L / R | A / S | LB / RB |
| C buttons | I J K L | right stick |
| D-pad | T F G H | D-pad |
| Menu | Esc | Back |
| Fast-forward (hold) | Tab | right trigger |

Debug keys in game: **P** pauses and resumes, **F9** writes a 4 MB RAM dump (big-endian, comparable with
Project64's) to `dumps\inbox\` if that folder exists, otherwise next to the exe.

Graphics default to the original N64 picture (4:3, the game's own frame rate, no anti-aliasing) at 2x resolution.
Higher resolutions ("Auto" in a large window) currently show thin lines across some scaled backgrounds.

## Repository overview

| Path | What |
|---|---|
| `splat\adam.yaml`, `splat\symbol_addrs.txt` | how the ROM is split into code sections; function names |
| `scripts\` | the pipeline (`rebuild.ps1`, `splat_to_toml.py`, `update_stubs.py`) and analysis tools (`disasm.py`, `who_calls.py`, `dl_dump.py`, ...) |
| `scripts\extra_hooks.txt`, `extra_stubs.txt`, ... | hand-written inputs for N64Recomp (hooks, stubs, renames) |
| `symbols.toml`, `config.toml` | N64Recomp input, generated by the pipeline (don't edit by hand) |
| `rsp\aspMain.toml` | RSPRecomp settings for the audio microcode |
| `runtime\src\` | the PC program: game registration, overlays, audio, input, settings |
| `runtime\src\live.cpp`, `rsp_interp.cpp` | the live build: recompiles the game code from the ROM at startup; audio microcode interpreter |
| `runtime\src\launcher_art.cpp` | decodes the launcher background from the stored ROM |
| `runtime\patches\` | fixes to RT64 (S2DEX2 2D graphics), RecompFrontend and N64ModernRuntime (fast-forward) |
| `runtime\assets\` | fonts and icons for the menus (built into `adam.exe`, unpacked into `data\` at startup) |
| `runtime\src\embedded_files_unpack.cpp` | unpacks the files built into the exe (list generated by `runtime\CMakeLists.txt`) |

`scripts\verify_setup.py` still checks the layout from before the overlays were mapped and reports outdated
"problems"; the other tools are current.

## Credits

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (MIT) and
  [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) (GPLv3) by Wiseguy and contributors
- [RT64](https://github.com/rt64/rt64) (MIT) by Dario and contributors
- [RecompFrontend](https://github.com/N64Recomp/RecompFrontend), the launcher/menu/input library (not included here;
  `runtime\patches\` contains small changes to it)
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
  [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) (GPLv3): reference integrations; parts of the
  audio output and the menu integration are adapted from them, and the menu icons come from Banjo: Recompiled
- [sljit](https://github.com/zherczeg/sljit) (BSD), the code generator inside N64Recomp's live recompiler
- [splat](https://github.com/ethteck/splat) and [rabbitizer](https://github.com/Decompollaborate/rabbitizer) (MIT),
  [n64sym](https://github.com/shygoo/n64sym) for identifying library functions
- Fonts: Inter and Noto Emoji (SIL Open Font License), promptfont (see `runtime\assets\promptfont\LICENSE.txt`)
- [Project64](https://www.pj64-emu.com/)

## License

GPLv3, see [LICENSE](LICENSE), because adam links with N64ModernRuntime (GPLv3) and contains code and assets
adapted from GPLv3 projects. The assets in `runtime\assets\` keep their own licenses (see its README).

RecompFrontend currently has no license file. adam uses it the same way other N64Recomp projects do. If its authors
or any other rights holder object to how it or anything else is used here, please open an issue and it will be
changed or taken down.

This project is not affiliated with or endorsed by Nintendo, Bandai Namco, or the rights holders of Neon Genesis
Evangelion. "Nintendo 64" and "Neon Genesis Evangelion" are trademarks of their respective owners and are used here
only to identify the game this project works with. It contains no copyrighted game data: no ROM, no game code, no
graphics, sound or text from the game.
