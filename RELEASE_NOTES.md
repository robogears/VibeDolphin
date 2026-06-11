# What's new in v0.1.2

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library.

## Wii Menu game launcher
- Your Wii disc games appear as **channel tiles** in the emulated Wii System Menu. Boot a game, press HOME → return to the Wii Menu, and pick another game right from the channel grid — no quitting back to Steam, no relaunching.
- Clicking a tile **boots the game straight away** (no "stop emulation?" prompt).
- Each game shows **its own banner icon**. The rare game whose banner would crash the menu falls back to a safe generic tile (a skip-list), so the menu never breaks.

## Automatic library sync
- On launch, the Wii Menu's channels **reconcile with your games folder** automatically — add or remove a game and the tiles follow, no manual step.
- **Tools → "Sync Wii Menu Channels"** forces a manual re-sync.
- A mappable **"Return to Wii Menu"** hotkey (Hotkey Settings → Wii) jumps back to the menu from any game — handy for a Steam Deck button.

## In this release
- The window title now reads **"VibeDolphin 0.1.2"**.
- VibeDolphin uses its **own user directory** (`~/.local/share/vibedolphin`), fully separate from any stock Dolphin install. Your config, NAND, saves, and game list **persist across updates and reinstalls** and never touch a stock Dolphin setup.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin-0.1.2-x86_64.AppImage`, make it executable (`chmod +x VibeDolphin-0.1.2-x86_64.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode.

VibeDolphin keeps everything under `~/.local/share/vibedolphin` — separate from stock Dolphin, and preserved when you replace/update the AppImage. It starts fresh on first run, so set your game paths and install a Wii System Menu in VibeDolphin.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (Tools → Perform Online System Update) — the channel tiles render inside it.
- Your Wii games in a folder configured under Config → Paths.

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.2
