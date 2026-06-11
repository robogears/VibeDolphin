# What's new in v0.1.8

VibeDolphin is a Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole Wii library. This release switches every channel tile to the game's **own real banner** and makes the menu **self-sorting**: a banner that crashes the channel grid is auto-detected and replaced with a placeholder, fully automatically.

## Real per-game banners for everything
- Every game now shows its **own real banner** (true icon + animation) — the most authentic art, instead of a re-hosted approximation. A one-time rebuild on first launch upgrades your existing tiles automatically.

## Crash-once, then auto-fix
- A few Wii games ship a banner the System Menu can't render (a long-standing "banner brick"). There's no way to know which without the menu trying — so VibeDolphin lets it crash **once**, **identifies which game** caused it, and on the next launch replaces just that game's tile with a yellow **"image not loaded"** placeholder (the game still launches). Every other tile keeps its real banner.
- Detected crashers are recorded in `wii_menu_quarantine.txt` in VibeDolphin's user folder — delete it to reset and try real banners again.

## No hardcoded assumptions + a loop backstop
- Removed the built-in list of "known-bad" games — nothing is pre-judged; the menu learns entirely from what actually crashes.
- Safety backstop: if the menu keeps crashing in a way that can't be pinned to one game, VibeDolphin falls back to safe placeholder tiles so you can never get stuck in a crash loop. (To retry real banners afterward, relaunch once with the `--generate-forwarders` launch option — it rebuilds every tile with its real banner.)

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths. (Include a common first-party game so the placeholder donor scene can be auto-captured.)

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.8
