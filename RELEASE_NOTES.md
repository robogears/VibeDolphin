# What's new in v0.1.1

First VibeDolphin release — a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library.

## Wii Menu game launcher
- Your Wii disc games now appear as **channel tiles** in the emulated Wii System Menu. Boot any game, press HOME → return to the Wii Menu, and pick another game right from the channel grid — no quitting back to Steam, no relaunching.
- Clicking a tile **boots the game straight away** (no "stop emulation?" prompt).
- Each game shows **its own banner icon**. The rare game whose banner would crash the menu falls back to a safe generic tile (a skip-list), so the menu never breaks.

## Automatic library sync
- On launch, the Wii Menu's channels **reconcile with your games folder** automatically — add or remove a game and the tiles follow, no manual step.
- **Tools → "Sync Wii Menu Channels"** forces a manual re-sync.
- A mappable **"Return to Wii Menu"** hotkey (Hotkey Settings → Wii) jumps back to the menu from any game — handy for a Steam Deck button.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin-0.1.1-x86_64.AppImage`, make it executable (`chmod +x VibeDolphin-0.1.1-x86_64.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch it from Game Mode.

VibeDolphin keeps its **own** config, NAND, and saves under `~/.local/share/vibedolphin` — completely separate from any stock Dolphin install, so it never touches your existing Dolphin setup. (It starts fresh, so on first run set your game paths and install a Wii System Menu in VibeDolphin.)

## Requirements
- A Wii **System Menu** must be installed in Dolphin's NAND (Tools → Perform Online System Update) — the channel tiles render inside it.
- Your Wii games in a folder configured under Dolphin's game paths (Config → Paths).

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.1
