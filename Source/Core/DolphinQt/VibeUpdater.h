// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VibeDolphin in-app self-updater.
//
// Checks the fork's GitHub Releases (robogears/VibeDolphin) for a newer published build, and -- on
// a writable Linux AppImage -- downloads the new AppImage, verifies its SHA-256 against GitHub's
// per-asset digest, swaps itself in place via a small detached helper, and relaunches. Networking
// reuses Dolphin's libcurl-backed Common::HttpRequest (TLS/CA/redirects already work on the Deck),
// so there is no extra TLS/JSON dependency. Everything degrades gracefully: a failed check is
// reported as "couldn't check" (never "up to date"), and a non-AppImage / read-only build falls
// back to opening the release page.

#pragma once

#include <optional>
#include <string>

class QWidget;

namespace VibeUpdate
{
struct Update
{
  std::string version;       // release tag, e.g. "0.1.14" (a leading 'v' is tolerated)
  std::string title;         // release name, for display
  std::string notes;         // release body (markdown), for display
  std::string html_url;      // release page (manual fallback)
  std::string asset_url;     // absolute browser_download_url of VibeDolphin.AppImage
  std::string asset_sha256;  // expected lowercase-hex SHA-256; empty if the release gave no digest
};

// NOTE: deliberately NOT named "Status" -- Xlib's <X11/Xlib.h> does `#define Status int`, which is
// pulled in transitively by the Qt platform headers in MainWindow.cpp and would turn
// "VibeUpdate::Status::Foo" into "VibeUpdate::int::Foo". CheckStatus sidesteps that landmine.
enum class CheckStatus
{
  UpToDate,
  UpdateAvailable,
  CheckFailed,  // network/parse failure -- distinct from "up to date"
};

struct Result
{
  CheckStatus status;
  std::optional<Update> update;
};

// The running build's numeric version (parsed from the branded version string), e.g. "0.1.13".
std::string CurrentVersion();

// BLOCKING GitHub query (call off the UI thread). Returns a tri-state result; CheckFailed on any
// network or parse error so a failed check is never misreported as up-to-date.
Result CheckForUpdate();

// True iff this build can replace itself in place: running from a writable Linux AppImage.
bool CanSelfInstall();

// Modal "update available" flow on the UI thread: shows the version + notes, and either downloads
// + verifies + self-installs + relaunches (writable AppImage), or opens the release page. Returns
// true iff a self-install was launched and the app is now quitting (QApplication::quit() called) --
// callers that have queued follow-up work (e.g. the kiosk pre-boot path about to boot the Wii Menu)
// should bail out so they don't run underneath the relaunch. Returns false otherwise (declined,
// release-page fallback, or a failed/canceled self-install -- the app keeps running).
bool ShowUpdateDialog(const Update& update, QWidget* parent);
}  // namespace VibeUpdate
