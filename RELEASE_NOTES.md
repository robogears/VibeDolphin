# What's new in v0.1.6

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library. This release is a robustness & polish pass (from a full code audit) on top of the 0.1.x feature set.

## More robust, especially on a fresh install
- **Crash-proofing now bootstraps itself.** The safe-banner system needs one known-good "donor" scene; VibeDolphin now **captures it automatically** from a common first-party game (Mario Kart Wii, Wii Sports / Resort, or New Super Mario Bros. Wii) in your library — no manual setup. If none can be built, a game is simply **skipped** (no tile) rather than risking the old menu crash.
- A corrupt or hand-edited channel map can no longer crash the emulator (robust parsing).

## Clearer feedback
- **Tools → "Sync Wii Menu Channels"** now reports what it did ("3 added, 1 removed", or "already up to date"), and tells you *why* if it can't run (stop emulation first / no System Menu installed).
- Booting the Wii Menu shows an **"Updating channels…"** splash so a first-run library scan doesn't look like a hang.
- If you launch into the Wii Menu with no System Menu installed, you now get a **clear message and the normal interface** instead of a generic error.

## Translucent tile art
- Game banners with transparency now keep it (no more dark fringe), and tile art quantization is slightly more accurate.

## Launch options
- Launches into the Wii Menu by default once one is installed. Use **`--gui`** (Steam Launch Options) or set **`VIBEDOLPHIN_FORCE_GUI=1`** to reach the normal Dolphin interface for settings.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths. (Include a common first-party game so the donor banner can be auto-captured.)

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.6
