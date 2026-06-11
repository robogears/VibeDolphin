# What's new in v0.1.9

Bug fix on top of v0.1.8's real-banner + auto-quarantine model.

## The auto-quarantine now actually runs on the Steam Deck
- **Fixed:** when launched straight into the Wii Menu (the Steam Deck / kiosk path — the normal way you launch it), the crash-detection window was never armed. So a game whose banner crashed the menu was **never detected, never quarantined, and crashed on every boot** (no `wii_menu_quarantine.txt` was written). The detection only worked from the desktop "Tools → Load Wii System Menu" path.
- Now the kiosk boot arms the same brick watchdog as the menu path: a crashing banner is caught, the game is identified and written to `wii_menu_quarantine.txt`, the menu stops cleanly, and on the next launch that one game becomes the yellow "image not loaded" tile while everything else keeps its real banner.

Everything else is unchanged from v0.1.8 (real per-game banners, crash-once-to-quarantine, loop-safety backstop, one-time migration).

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths. (Include a common first-party game so the placeholder donor scene can be auto-captured.)

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.9
