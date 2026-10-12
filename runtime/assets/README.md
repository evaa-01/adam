# UI assets

Fonts, icons and the (empty) stylesheet RecompFrontend's menus load from `assets\` next to adam.exe
(runtime\CMakeLists.txt copies this folder there). Taken from Banjo: Recompiled (GPLv3,
https://github.com/BanjoRecomp/BanjoRecomp, commit dbd5215), without its game artwork:

- `icons\` - UI icons (GPLv3, as Banjo: Recompiled)
- `InterVariable.ttf` - Inter, SIL Open Font License; registered as the primary font ("Inter Variable")
- `NotoEmoji-Regular.ttf` - Noto Emoji, SIL Open Font License
- `promptfont\` - controller button glyphs, see its LICENSE.txt
- `recomp.rcss` - placeholder; RmlUi needs at least one rule in it

`..\recompcontrollerdb.txt` (SDL game controller mappings) comes from the same place.
