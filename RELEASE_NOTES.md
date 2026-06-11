# What's new in v0.1.4

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library.

## Boots straight into the Wii Menu
- Launching VibeDolphin now drops you **right into the emulated Wii System Menu, fullscreen** — no Dolphin desktop UI in between. On the Steam Deck, add it to Steam and launching it lands you on the channel grid, ready to pick a game.
- The channel tiles are **refreshed before the menu opens**, so games you've added or removed show up immediately.
- Need the normal Dolphin interface (to set game paths, install the System Menu, or change settings)? Launch with the **`--gui`** option (in Steam: Properties → Launch Options → `--gui`). The first run, before a System Menu is installed, also opens the normal interface automatically.

## Carried over from 0.1.3
- **Real per-game channel art that can't crash the menu** — each game's artwork is re-hosted in a safe channel scene.
- **Self-healing Wii Menu** — if a banner ever bricks the menu, it's suppressed and the channels are rebuilt safely on the next launch.

## Also in this release
- The window title now reads **"VibeDolphin 0.1.4"**.
- Separate, persistent user directory (`~/.local/share/vibedolphin`) that survives updates and reinstalls and never touches a stock Dolphin install.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui` (or run the AppImage with `--gui` from a terminal). VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update) — the channel tiles render inside it, and it's what the kiosk boots into.
- Your Wii games in a folder configured under Config → Paths.

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.4
