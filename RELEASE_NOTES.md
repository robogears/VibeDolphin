# What's new in v0.1.12

**Two big things: VibeDolphin can update itself now, and banner handling got a lot simpler.**

## Built-in updater — no more hunting for the latest AppImage
- **Help → "Check for VibeDolphin Updates…"** queries the GitHub releases and, if there's a newer build, offers to grab it. On the Steam Deck the same check runs **once, right before the Wii Menu boots**, so you're offered the update at the one calm moment before gameplay.
- **One click to update.** On the AppImage it downloads the new build, **verifies its SHA-256 checksum** before trusting it, swaps itself in place, and relaunches — straight back into the Wii Menu. No terminal, no `chmod`, no re-adding to Steam.
- **Safe by design.** A failed check is reported as "couldn't check," never silently treated as up-to-date. The download is checksum-verified before it's ever made executable. If anything can't self-install (e.g. a read-only location), it just opens the releases page instead.

> **One-time manual step:** the updater ships *in* this build, so it can only update you to releases that come *after* it. Download this one by hand; from here on, VibeDolphin keeps itself current.

## Banner handling is just a blacklist now
All the automatic crash-detection, "safe mode," migration markers, and self-heal machinery is **gone** — no more `forwarder_safe_mode`, `forwarder_regen_pending`, `forwarder_realart_migrated`, or `wii_menu_quarantine.txt`, and nothing turns your banners into the Mario Kart donor en masse.

- **Every game shows its own real banner.**
- **A blocklisted game shows the yellow "image not loaded" tile.** The blocklist = a built-in entry for Mario Party 9 (all regions) + your own `forwarder_blocklist.txt` (one game ID per line).
- Each launch rebuilds the channel tiles from that blocklist, so it's always consistent — no stale state, no markers, nothing to clean up.

If some other game ever crashes the menu, just add its game ID to `forwarder_blocklist.txt` in `~/.local/share/vibedolphin/` and relaunch — it becomes a yellow tile, everything else keeps its real banner.

---

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
