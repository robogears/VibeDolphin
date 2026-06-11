// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"

// Small utility functions for common Wii related tasks.

namespace DiscIO
{
class VolumeWAD;
}

namespace IOS::HLE
{
class BluetoothEmuDevice;
class ESCore;
class Kernel;
}  // namespace IOS::HLE

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace WiiUtils
{
enum class InstallType
{
  Permanent,
  Temporary,
};

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType type);
// Same as the above, but constructs a temporary IOS and VolumeWAD instance for importing
// and does a permanent install.
bool InstallWAD(const std::string& wad_path);

// Forwarder channels: fake-signed Wii channel titles that appear as tiles in the
// emulated Wii System Menu; their launch is intercepted by the ES hook and
// redirected to boot the mapped disc image (see SetForwarderBootHandler).
struct ForwarderInfo
{
  u64 title_id;
  std::string game_id;
  u16 revision;
};
struct ForwarderLibraryEntry
{
  std::string disc_path;
  std::string long_name;
};

// Deterministic, file-location-independent forwarder title id for a disc identity.
u64 ComputeForwarderTitleId(const std::string& game_id, u16 revision);
// True if title_id is in our forwarder-channel namespace.
bool IsForwarderTitle(u64 title_id);

// Build + install one forwarder for a Wii disc image (uses the disc's own
// opening.bnr as the banner). Returns its info, or nullopt (non-Wii / no banner).
std::optional<ForwarderInfo> InstallForwarder(const std::string& disc_path);
// Install forwarders for a whole library and persist the title-id -> path map.
// Returns the number installed.
size_t InstallForwardersForLibrary(const std::vector<ForwarderLibraryEntry>& games);

// Remove a forwarder channel from NAND (title dir + ticket). No-op + false for any
// title id outside our forwarder namespace (never deletes real titles).
bool UninstallForwarder(u64 title_id);

struct ForwarderSyncResult
{
  size_t installed = 0;
  size_t already_present = 0;
  size_t moved = 0;
  size_t uninstalled = 0;
  size_t orphaned = 0;
  bool map_modified = false;
};
// Incrementally reconcile installed forwarders with the given set of disc paths:
// install newly-added games, drop removed ones, follow moved/renamed files, and
// persist the map only if it changed. Safe to call off the UI thread; runs at most
// one reconcile at a time (extra concurrent calls return immediately).
ForwarderSyncResult SyncForwardersWithLibrary(const std::vector<std::string>& current_disc_paths);

// Look up a forwarder's disc path (lazy-loads forwarders.json); used by the ES hook.
std::optional<std::string> LookupForwarderDiscPath(u64 title_id);
// Invalidate the cached map so the next lookup reloads (after (re)generating).
void ReloadForwarderMap();

// (DEV/spike) Forwarder launch -> host disc boot bridge. The frontend registers a
// handler that boots the given disc image (replacing the current session); the ES
// launch interception calls RequestForwarderBoot when a forwarder title launches.
void SetForwarderBootHandler(std::function<void(const std::string& disc_path)> handler);
void RequestForwarderBoot(const std::string& disc_path);

bool UninstallTitle(u64 title_id);

bool IsTitleInstalled(u64 title_id);

// Checks if there's a title.tmd imported for the given title ID.
bool IsTMDImported(IOS::HLE::FS::FileSystem& fs, u64 title_id);

// Searches for a TMD matching the given title ID in /title/00000001/00000002/data/tmds.sys.
// Returns it if it exists, otherwise returns an empty invalid TMD.
IOS::ES::TMDReader FindBackupTMD(IOS::HLE::FS::FileSystem& fs, u64 title_id);

// Checks if there's a title.tmd imported for the given title ID. If there is not, we attempt to
// re-import it from the TMDs stored in /title/00000001/00000002/data/tmds.sys.
// Returns true if, after this function call, we have an imported title.tmd, or false if not.
bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id);

enum class UpdateResult
{
  Succeeded,
  AlreadyUpToDate,

  // Current region does not match disc region.
  RegionMismatch,
  // Missing update partition on disc.
  MissingUpdatePartition,
  // Missing or invalid files on disc.
  DiscReadFailed,

  // NUS errors and failures.
  ServerFailed,
  // General download failures.
  DownloadFailed,

  // Import failures.
  ImportFailed,
  // Update was cancelled.
  Cancelled,

  NumberOfEntries,
};

// Return false to cancel the update as soon as the current title has finished updating.
using UpdateCallback = std::function<bool(size_t processed, size_t total, u64 title_id)>;
// Download and install the latest version of all titles (if missing) from NUS.
// If no region is specified, the region of the installed System Menu will be used.
// If no region is specified and no system menu is installed, the update will fail.
UpdateResult DoOnlineUpdate(UpdateCallback update_callback, const std::string& region);

// Perform a disc update with behaviour similar to the System Menu.
UpdateResult DoDiscUpdate(UpdateCallback update_callback, const std::string& image_path);

// Check the emulated NAND for common issues.
struct NANDCheckResult
{
  bool bad = false;
  std::unordered_set<u64> titles_to_remove;
  u64 used_clusters_user = 0;
  u64 used_clusters_system = 0;
  u64 used_inodes_user = 0;
  u64 used_inodes_system = 0;
};
NANDCheckResult CheckNAND(IOS::HLE::Kernel& ios);
bool RepairNAND(IOS::HLE::Kernel& ios);

// Get the BluetoothEmuDevice for an active emulation instance.
// It is only safe to call this from the CPU thread.
// Returns nullptr if we're not currently emulating a Wii game or if Bluetooth passthrough is used.
std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice();
// Same as GetBluetoothEmuDevice, but for Bluetooth passthrough.
std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice();
}  // namespace WiiUtils
