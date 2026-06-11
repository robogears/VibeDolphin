# What's new in v0.1.11

This release makes the menu-crash handling reliable and ships Mario Party 9 pre-flagged.

## Mario Party 9 just works now
- **MP9 (all regions) is now a built-in "image not loaded" tile** — it's the one game confirmed to crash the Wii Menu's channel grid, so it ships pre-flagged. No setup, no crash; every other game shows its real banner.

## Reliable, predictable crash handling (no more wrong-game guessing or loops)
- The old auto-detection tried to guess *which* game crashed from "the last banner read" — but the System Menu reads **all** banners before drawing them, so that guess was wrong (it could flag an innocent game while the real culprit kept crashing). **That guessing is removed.**
- Now, if an *unflagged* game ever crashes the menu, VibeDolphin switches to safe placeholder tiles so the menu always boots cleanly — and the message tells you to add that game's ID to `forwarder_blocklist.txt` to give it a caution tile (every other game keeps its real banner).
- **Fixed an infinite-crash loop:** the desktop "Load Wii System Menu" path now applies a pending heal *before* booting (the Steam Deck path already did), so a crash can never get stuck repeating.

## Flag your own bad games
- `forwarder_blocklist.txt` in your VibeDolphin user folder (`~/.local/share/vibedolphin/`) — one game ID per line — gives any game a yellow "image not loaded" tile. This is the same approach the Wii homebrew scene uses.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Updating from an earlier build
If MP9 was already installed as a real-banner tile, delete `forwarder_realart_migrated` (and `forwarder_safe_mode` if present) from `~/.local/share/vibedolphin/` once, then relaunch — VibeDolphin rebuilds the tiles with MP9 as the caution tile and everything else as its real banner.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths.

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.11
