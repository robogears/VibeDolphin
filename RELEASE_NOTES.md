# What's new in v0.1.3

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library.

## Real per-game channel art — and it can't crash the menu
- Every game's tile now shows **its own banner artwork**. The artwork is re-hosted inside a known-safe channel scene (only the texture is swapped; the layout/animation that the System Menu renders is left untouched), so a game whose banner used to crash the Wii Menu's channel grid **no longer can**.
- Games that previously bricked the menu (e.g. Mario Party 9) now appear and launch normally.

## Self-healing Wii Menu (never get stuck on a crash)
- In the unlikely event a banner still bricks the menu, VibeDolphin **suppresses the crash and stops cleanly with a message** instead of leaving you on a black screen.
- On the next launch (or **Tools → "Sync Wii Menu Channels"**), it automatically rebuilds every channel with a safe banner so the menu boots fine.

## Also in this release
- The window title now reads **"VibeDolphin 0.1.3"**.
- Carries over from 0.1.2: the Wii Menu game launcher, automatic library sync, the mappable **"Return to Wii Menu"** hotkey, and a **separate, persistent user directory** (`~/.local/share/vibedolphin`) that survives updates and reinstalls and never touches a stock Dolphin install.

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin-0.1.3-x86_64.AppImage`, make it executable (`chmod +x VibeDolphin-0.1.3-x86_64.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode.

VibeDolphin keeps everything under `~/.local/share/vibedolphin` — separate from stock Dolphin, and preserved when you replace/update the AppImage. It starts fresh on first run, so set your game paths and install a Wii System Menu in VibeDolphin.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (Tools → Perform Online System Update) — the channel tiles render inside it.
- Your Wii games in a folder configured under Config → Paths.

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.3
