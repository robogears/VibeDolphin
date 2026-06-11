// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VibeDolphin: crash-proof per-game channel banners.
//
// The Wii System Menu's channel grid does no error handling on banner icon assets:
// certain disc games' opening.bnr icon scenes (BRLYT/BRLAN/TPL) make it null-deref and
// crash ("banner brick", PC=0x8136a488). We can't statically tell which banners are bad,
// so instead of reusing a disc banner's scene verbatim we re-host the game's *artwork* in
// a known-safe donor scene: keep the donor's BRLYT/BRLAN byte-for-byte (so the renderer
// walks a structure it already handles) and swap only the pixel data of the donor's
// largest texture with the game's own art, resized to the donor texture's exact
// format/dimensions. The structure the renderer touches never changes, so it can't brick.

#pragma once

#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace WiiUtils::Banner
{
// Build a System-Menu-safe channel banner that shows the game's own art.
//
// Starts from |donor_banner| (a full opening.bnr/channel banner known to render safely),
// patches in the game's IMET title text from |game_opening_bnr|, and swaps the game's
// artwork into the donor's icon (and banner) scene textures. The result is validated by
// re-parsing it; if art extraction or any structural step fails, this still returns a
// valid banner equal to "donor scene + game titles" (never worse than the plain donor
// fallback). Returns nullopt only if |donor_banner| itself is unusable.
std::optional<std::vector<u8>> BuildArtChannelBanner(const std::vector<u8>& donor_banner,
                                                     const std::vector<u8>& game_opening_bnr);

// Build a System-Menu-safe channel banner that shows a yellow "image not loaded" caution
// placeholder instead of real art, for a game whose own banner crashes the System Menu.
// Reuses |donor_banner|'s proven-safe scene (so it can't brick) but paints a warning-triangle
// placeholder into the icon/banner texture slots; the game's IMET title text is still patched in
// from |game_opening_bnr| so the tile keeps the correct game name. Falls back to "donor scene +
// game titles" if the placeholder can't be painted; nullopt only if |donor_banner| is unusable.
std::optional<std::vector<u8>> BuildCautionBanner(const std::vector<u8>& donor_banner,
                                                  const std::vector<u8>& game_opening_bnr);

// Round-trips every binary primitive (LZ10 (de)compress, U8 parse/repack, IMD5 wrap,
// TPL parse, RGB5A3/RGBA8 encode->decode). Returns true iff all self-checks pass. Used as
// a startup sanity gate so a packing regression disables art banners instead of shipping
// a malformed one. Logs the first failure.
bool RunSelfTests();
}  // namespace WiiUtils::Banner
