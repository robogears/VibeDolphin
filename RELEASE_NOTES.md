# What's new in v0.1.5

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library.

## Tidy channel grid — no gaps, consistent order
- Your game tiles now **stack evenly** in the Wii Menu. Remove a game and the rest close up — no leftover empty slots — and tiles no longer land in random spots.
- This happens automatically on every launch (as part of the auto-sync), right before the menu opens. System channels (Disc Channel, etc.) stay first; your game tiles follow in a stable order.

## Carried over from 0.1.4
- **Boots straight into the Wii Menu**, fullscreen (pass `--gui` for the normal interface).
- **Scans + syncs your library every launch** — added/removed games update automatically.
- **Real per-game channel art that can't crash the menu**, plus the self-healing safety net.

## Also in this release
- The window title now reads **"VibeDolphin 0.1.5"**.
- The AppImage is named simply `VibeDolphin.AppImage`, with its own persistent user directory (`~/.local/share/vibedolphin`) that survives updates.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui` (or run the AppImage with `--gui` from a terminal). VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update) — the channel tiles render inside it, and it's what the kiosk boots into.
- Your Wii games in a folder configured under Config → Paths.

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.5
