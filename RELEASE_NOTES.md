# What's new in v0.1.10

Bug fix on top of v0.1.9.

## A crashing banner is now caught cleanly (no error flood)
- A bad banner doesn't always fault just once — it can send the System Menu's renderer into a garbage-pointer loop that spews **hundreds** of memory-fault panics in a row. v0.1.9 caught the crash but only handled the *first* panic well; the flood then (a) leaked raw error dialogs and (b) — because the first panic consumes the "which game" marker — made every following panic think the culprit was unknown and trip the all-tiles-to-placeholder safety fallback.
- **Fixed:** VibeDolphin now acts on the first brick panic only (records the culprit, quarantines that one game), and **suppresses the entire rest of the flood** while it stops the wedged menu — so you get one clean "a channel banner crashed" notice instead of a wall of errors, and only the actual offending game is quarantined.

After this, a crashing game should: crash once (caught + quarantined silently), then come back as the yellow "image not loaded" tile on the next launch, with every other game keeping its real banner.

Everything else is unchanged from v0.1.8/0.1.9 (real per-game banners, crash-once-to-quarantine, loop-safety backstop, one-time migration).

---

# Install

- **Linux / Steam Deck (x86_64)**: download `VibeDolphin.AppImage`, make it executable (`chmod +x VibeDolphin.AppImage`), and run it. On the Steam Deck, add it to Steam as a non-Steam game and launch from Game Mode — it boots straight into the Wii Menu.

To reach the normal Dolphin interface (first-time setup or settings), set the Steam **Launch Options** to `--gui`. VibeDolphin keeps everything under `~/.local/share/vibedolphin`, separate from stock Dolphin and preserved across updates.

## Requirements
- A Wii **System Menu** installed in VibeDolphin's NAND (run with `--gui`, then Tools → Perform Online System Update).
- Your Wii games in a folder configured under Config → Paths. (Include a common first-party game so the placeholder donor scene can be auto-captured.)

---

**Full Changelog**: https://github.com/robogears/VibeDolphin/commits/v0.1.10
