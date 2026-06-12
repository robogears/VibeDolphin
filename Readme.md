# VibeDolphin

**A Dolphin fork that turns the emulated Wii System Menu into a launcher for your whole game library — every Wii disc game shows up as its own channel tile, and clicking it boots the game. Built for the Steam Deck.**

VibeDolphin is a fork of the [Dolphin](https://dolphin-emu.org/) GameCube/Wii emulator. It ships as a single Linux x86‑64 **AppImage** and is designed to be added to Steam as a non‑Steam game: launch it from Game Mode and you drop straight into the **Wii System Menu 4.3**, where your games are waiting as channels — just like a real Wii.

---

## ⚠️ Read this first: VibeDolphin is 100% AI‑built

**Every line of VibeDolphin's custom code was written by AI (Claude), with little to no line‑by‑line human review.** The human "developer" directed the work and tested builds on hardware, but did not hand‑write or formally audit the code. **Treat this project with caution:**

- **Expect bugs.** This fork modifies low‑level emulator internals — it writes to the emulated Wii **NAND**, installs **fake‑signed "forwarder" channel titles**, hooks the title‑launch path, and **replaces its own binary** when self‑updating. Mistakes in any of those can corrupt your NAND, your save data, or the app itself.
- **Back up your data.** Before using it, back up your Wii NAND and saves. Keep your real game backups somewhere VibeDolphin doesn't touch.
- **It downloads and runs code to update itself.** The built‑in updater fetches a new AppImage from this project's GitHub Releases and executes it (after a SHA‑256 check). If you're not comfortable with that, don't use the updater.
- **Not affiliated with or endorsed by the Dolphin project.** Don't report VibeDolphin issues to upstream Dolphin. Don't assume upstream's quality or security guarantees apply here.
- **Use at your own risk.** No warranty. If something breaks, you get to keep both pieces.

If that's not for you, use upstream [Dolphin](https://dolphin-emu.org/) instead — it's excellent, human‑maintained, and battle‑tested.

---

## The main idea: your games, as channels, in the Wii Menu

On a real Wii, you turn it on and see a grid of channels. VibeDolphin recreates that feeling for your *whole library*:

1. You boot VibeDolphin (or launch it from Steam) and land in the emulated **Wii System Menu 4.3**.
2. **Every Wii disc game in your library appears as a channel tile**, showing the game's own banner art.
3. **Click a tile and that game boots** — no quitting back to Steam, no relaunching the emulator. When you're done, return to the Wii Menu and pick another game.

It's the cozy, console‑like front end the Wii never let you have for your backups — and it's especially at home on the Steam Deck in Game Mode with a controller.

### How it works (the short version)

Wii **disc** games (`.wbfs` / `.iso` / `.rvz`) can't normally appear in the Wii Menu — they run off the emulated disc drive and have no NAND channel. VibeDolphin bridges that gap:

- For each game in your library it installs a tiny **forwarder channel** — a real, fake‑signed NAND title that shows up as a tile with the game's banner.
- A small **hook in Dolphin's title‑launch path** notices when the Menu tries to launch one of these forwarders and instead reboots the emulator straight into the mapped disc image.
- A persistent map ties each forwarder's title ID to a disc file, and the channels are re‑synced from your library so the grid stays current.

The result: clicking a tile boots the right game, and you can bounce between games entirely inside the Wii Menu.

## Features

- **Wii Menu game launcher** — your disc games as channel tiles, with their real banner art.
- **Steam Deck kiosk mode** — launched with no arguments (e.g. from Steam Game Mode), VibeDolphin boots **straight into the Wii Menu** once a System Menu is installed. Pass `--gui` (or set `VIBEDOLPHIN_FORCE_GUI=1`) to get the normal Dolphin interface for setup and settings.
- **"Sync Wii Menu Channels"** — a Tools‑menu action (run automatically each launch) that reconciles the channel tiles with your current game library.
- **Caution tiles for problem banners** — a few games' banners crash the Menu's channel grid. Such games are shown as a yellow **"image not loaded"** caution tile (the launch shortcut still works); every other game keeps its real banner. The blocklist is a built‑in entry for Mario Party 9 plus your own `forwarder_blocklist.txt` (one game ID per line) in the user folder.
- **Built‑in self‑updater** — **Help → "Check for VibeDolphin Updates…"** checks GitHub Releases and, on the AppImage, downloads the new build, verifies its SHA‑256, swaps itself in place, and relaunches. A failed check is reported as "couldn't check," never silently treated as up‑to‑date.
- **Everything Dolphin already does** — VibeDolphin is a normal Dolphin underneath; all the usual emulation, graphics, controller, and tooling features are intact.

VibeDolphin keeps its own user folder at `~/.local/share/vibedolphin`, **separate** from any stock Dolphin install and preserved across updates.

## Requirements

- **Linux x86‑64** (the AppImage target; developed and tested on the **Steam Deck**).
- A Wii **System Menu** installed in VibeDolphin's NAND. Run with `--gui`, then **Tools → Perform Online System Update** — the channel tiles render *inside* the System Menu, so one must be installed.
- Your Wii game backups in a folder configured under **Config → Paths**.
- You provide your own games and system files. VibeDolphin includes no copyrighted Nintendo content.

## Install & use (Steam Deck)

1. Download `VibeDolphin.AppImage` from the [Releases page](https://github.com/robogears/VibeDolphin/releases) into a **writable** location (e.g. `~/Applications`).
2. Make it executable: `chmod +x VibeDolphin.AppImage`.
3. First‑time setup (Desktop Mode): run it with `--gui`, install a Wii System Menu (**Tools → Perform Online System Update**), and set your game folder under **Config → Paths**.
4. Add `VibeDolphin.AppImage` to Steam as a **non‑Steam game**. Launch it from Game Mode and it boots into the Wii Menu with your games as channels. (Set Steam **Launch Options** to `--gui` if you ever need the normal interface from Game Mode.)

## Updating

Use **Help → "Check for VibeDolphin Updates…"** (the Steam Deck kiosk also checks once before booting the Menu). Because the updater ships *inside* the app, the **first** build must be downloaded by hand once; after that VibeDolphin keeps itself current. If an update can't be applied (e.g. a read‑only location), it falls back to opening the Releases page.

## Building from source

VibeDolphin builds like Dolphin. To produce the Linux AppImage, use the bundled script on a Linux x86‑64 machine with the Dolphin build dependencies installed:

```sh
git submodule update --init --recursive
Distribution/build-appimage.sh        # writes VibeDolphin.AppImage in the repo root
```

The script lists the exact `apt` dependencies in its header. For general (non‑AppImage) builds on Linux, macOS, Windows, or Android, follow upstream Dolphin's instructions — see the [Dolphin wiki](https://github.com/dolphin-emu/dolphin/wiki) and `Contributing.md`. In short, on Linux/macOS:

```sh
mkdir build && cd build
cmake ..
make -j $(nproc)
```

## Relationship to Dolphin, credits & license

VibeDolphin is a downstream fork of the [Dolphin Emulator](https://github.com/dolphin-emu/dolphin). All of the heavy lifting — the actual GameCube/Wii emulation — is Dolphin's work by the Dolphin team and contributors. VibeDolphin only adds the Wii‑Menu launcher, the Steam Deck kiosk flow, the caution‑tile banner handling, and the self‑updater on top.

VibeDolphin is licensed under the **GNU General Public License, version 2 or later (GPLv2+)**, the same as Dolphin. See `COPYING` and `LICENSES/`.

Upstream Dolphin resources: [Homepage](https://dolphin-emu.org/) · [Project Site](https://github.com/dolphin-emu/dolphin) · [Wiki](https://wiki.dolphin-emu.org/) · [FAQ](https://dolphin-emu.org/docs/faq/) · [Issue Tracker](https://bugs.dolphin-emu.org/projects/emulator/issues)

> Again: **this fork's code is AI‑generated and lightly reviewed.** Please report problems with VibeDolphin to *this* repository, never to upstream Dolphin, and keep backups.
