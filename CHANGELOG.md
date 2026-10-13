# Changelog

What changed in each download. Newest first. Version numbers start with 0.1.0; earlier downloads were named by
date (`adam-<date>-<commit>.zip`).

## Unreleased

Nothing yet.

## 0.1.1 (2026-10-13)

Same game as 0.1.0; only the version number changed.

### Changed

- The launcher shows the download's version number (0.1.1), so the two always match.

## 0.1.0 (2026-10-13)

Changes since the previous download (`adam-2026-10-12-9f7f668.zip`).

### New

- **Rewind**: hold Backspace or click the left stick during a mission to run the game backwards, up to 10 seconds.
  Let go and the game continues from there. While the game's own pause screen is open, rewind jumps straight back
  into the action. On/off in Settings -> General (on by default). Only in missions: intro, menus and cutscenes
  ignore it.
- **Bezels**: artwork from the game in the black bars beside the 4:3 picture. New Settings -> Bezels tab with a
  preview of every usable picture from your ROM (93), or Off. Adjust brightness, black & white, and position and
  size (press A / Enter on the control, then move with the d-pad, left stick or arrow keys; resize with the right
  stick or + / -). The pictures are read from your ROM; the download contains none of them.
- **Launcher background**: the launcher has a new "Background" entry with the same picker and adjustments as the
  bezels (or Off for the plain look).
- **Reset Game**: button in Settings -> General (asks first). Restarts the game; saves are kept.
- **Fast-forward and rewind can be remapped** in Settings -> Controls (rows after the D-pad).
- The download's name and `HOW_TO_PLAY.txt` now show a version number.

### Changed

- **Fast-forward is a fixed 2x** with an on/off option in Settings -> General. The 2x / 4x / 8x choice is gone:
  4x and 8x flickered a lot.
- **Widescreen only in gameplay**: if you choose a wide aspect ratio in Settings -> Graphics, it is now used only in
  mission battles. Intro, menus, cutscenes and briefings stay 4:3 (they are drawn for 4:3 and showed empty or
  stretched edges). Your saved setting doesn't change.
- Company names were removed from the included texts.

### Fixed

- Fast-forward: a half-drawn frame could be shown (HUD missing or a black screen flickering in missions).
- Fast-forward at 8x could freeze the game while held.
- Rumble no longer keeps running while a menu is open, the game is paused or you rewind.

### Known issues

- Higher render resolutions ("Auto" in a large window) show thin lines across some scaled backgrounds. The default
  (original 2x) doesn't.
- Widescreen in battles hasn't been checked for objects disappearing at the new screen edges.
