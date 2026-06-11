# What's new in v0.1.7

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library. This release makes the menu **self-healing per game**: a single game whose banner crashes the channel grid no longer takes the whole menu down — it's automatically flagged and replaced with a clear placeholder, while every other tile keeps its real art.

## Auto-quarantine for menu-crashing banners
- A few Wii games ship a disc banner that the System Menu's channel grid can't render and **crashes on** (a long-standing "banner brick" — there's no way to detect it without the menu trying). VibeDolphin now **detects which game caused the crash**, records it, and on the next launch rebuilds **only that game's tile** as a yellow **"image not loaded"** caution placeholder. The tile keeps the correct game name and **still launches the game** — your other channels are untouched and keep their real art.
- The list of flagged games lives in a plain-text file, **`wii_menu_quarantine.txt`**, in VibeDolphin's user folder. **Delete it to reset** and try real art again for everything.

## Per-game art re-enabled
- Fixed an internal self-test mismatch that was silently disabling the banner toolkit, so newly added games again get their **own per-game tile art** (now including the common CMPR texture format), with the crash-proof safe fallback intact.

## Under the hood
- The crash self-heal now quarantines a single attributed game instead of switching every tile to a generic donor; the generic safe-mode fallback only kicks in if a crash can't be pinned to one game, and it's no longer "sticky" — it clears itself after one heal.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths. (Include a common first-party game so the donor banner can be auto-captured.)

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.7
