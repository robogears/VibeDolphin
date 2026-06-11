// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/md5.h>
#include <pugixml.hpp>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"
#include "Core/IOS/Uids.h"
#include "Core/SysConf.h"
#include "Core/System.h"
#include "Core/WiiBanner.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeFileBlobReader.h"
#include "DiscIO/VolumeWad.h"

namespace WiiUtils
{
namespace
{
// Frontend-registered handler that boots a disc image (replacing the current
// session) when a forwarder channel is launched. Null in non-GUI link targets.
std::function<void(const std::string&)> s_forwarder_boot_handler;

// In-memory forwarder title-id -> disc-path map, lazily loaded from forwarders.json.
std::map<u64, std::string> s_forwarder_map;
std::mutex s_forwarder_map_mutex;
bool s_forwarder_map_loaded = false;

// Guards SyncForwardersWithLibrary against re-entrancy / concurrent runs.
std::atomic<bool> s_forwarder_sync_running{false};

// Crash safety net. s_safe_banner_mode forces plain-donor banners (no per-game art).
// s_wii_menu_boot_pending is true only while the emulated System Menu is the running
// session, so a null-read panic in that window is attributed to the channel grid.
// s_wii_menu_brick_detected is the in-session flag the frontend polls to stop + notify.
// s_brick_handled_this_boot makes the first brick panic the only one we ACT on: a bad banner
// often spews a flood of faults (a garbage-pointer loop), and we must quarantine the culprit
// exactly once and suppress the rest -- not re-attribute (the marker is consumed on the first
// read, so re-entry would wrongly trip the "unknown culprit -> blanket safe mode" backstop).
std::atomic<bool> s_safe_banner_mode{false};
std::atomic<bool> s_wii_menu_boot_pending{false};
std::atomic<bool> s_wii_menu_brick_detected{false};
std::atomic<bool> s_brick_handled_this_boot{false};

// Persistent markers under the user dir: SAFE_MODE makes safe-banner mode sticky across
// launches; REGEN_PENDING is the one-shot "rebuild now" flag set on a brick.
const char* const SAFE_MODE_MARKER = "forwarder_safe_mode";
const char* const REGEN_PENDING_MARKER = "forwarder_regen_pending";

// Parse a hex title id from a (possibly hand-edited / truncated) JSON string. Non-throwing
// (Core is built with -fno-exceptions, so std::stoull on bad input would terminate); returns
// nullopt to skip the row. LookupForwarderDiscPath runs on the emulation/CPU thread, so a
// crash here would take down emulation.
std::optional<u64> ParseHexTitleId(const std::string& s)
{
  u64 value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
  if (ec != std::errc{} || ptr != s.data() + s.size())
    return std::nullopt;
  return value;
}

// A full forwarders.json row (the lazy lookup map only retains title_id -> disc_path).
struct ForwarderMapEntry
{
  u64 title_id = 0;
  std::string game_id;
  u16 revision = 0;
  std::string disc_path;
};

// Parse every field of forwarders.json (unlike the title_id+disc_path-only lazy loader).
// Returns an empty list for a missing/corrupt file (treated as a fresh install).
std::vector<ForwarderMapEntry> LoadForwarderMapFull()
{
  std::vector<ForwarderMapEntry> entries;
  const std::string path = File::GetUserPath(D_USER_IDX) + "forwarders.json";
  picojson::value root;
  std::string error;
  if (!JsonFromFile(path, &root, &error) || !root.is<picojson::object>())
    return entries;
  const picojson::object& obj = root.get<picojson::object>();
  const auto forwarders = obj.find("forwarders");
  if (forwarders == obj.end() || !forwarders->second.is<picojson::array>())
    return entries;
  for (const picojson::value& v : forwarders->second.get<picojson::array>())
  {
    if (!v.is<picojson::object>())
      continue;
    const picojson::object& row = v.get<picojson::object>();
    const std::optional<std::string> tid = ReadStringFromJson(row, "title_id");
    const std::optional<std::string> disc = ReadStringFromJson(row, "disc_path");
    if (!tid || !disc)
      continue;
    const std::optional<u64> title_id = ParseHexTitleId(*tid);
    if (!title_id)
    {
      WARN_LOG_FMT(IOS_ES, "Forwarder map: bad title_id '{}'; skipping row", *tid);
      continue;
    }
    ForwarderMapEntry e;
    e.title_id = *title_id;
    e.disc_path = *disc;
    e.game_id = ReadStringFromJson(row, "game_id").value_or("");
    const auto rev = row.find("revision");
    if (rev != row.end() && rev->second.is<double>())
      e.revision = static_cast<u16>(rev->second.get<double>());
    entries.push_back(std::move(e));
  }
  return entries;
}

// Serialize the forwarder title_id -> disc_path map to forwarders.json.
bool WriteForwarderMap(const std::vector<ForwarderMapEntry>& entries)
{
  picojson::array rows;
  for (const ForwarderMapEntry& e : entries)
  {
    picojson::object row;
    row["title_id"] = picojson::value(fmt::format("{:016x}", e.title_id));
    row["game_id"] = picojson::value(e.game_id);
    row["revision"] = picojson::value(static_cast<double>(e.revision));
    row["disc_path"] = picojson::value(e.disc_path);
    rows.emplace_back(std::move(row));
  }
  picojson::object doc;
  doc["version"] = picojson::value(1.0);
  doc["forwarders"] = picojson::value(rows);
  return JsonToFile(File::GetUserPath(D_USER_IDX) + "forwarders.json", picojson::value(doc), true);
}

// Retail Wii common key (identical to the one in IOSC). Used to AES-128-CBC
// encrypt the forwarder's title key so ES can decrypt it during import.
constexpr std::array<u8, 16> RETAIL_COMMON_KEY = {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
                                                  0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};

void WriteBE16(std::vector<u8>& b, size_t off, u16 v)
{
  b[off] = static_cast<u8>(v >> 8);
  b[off + 1] = static_cast<u8>(v);
}
void WriteBE32(std::vector<u8>& b, size_t off, u32 v)
{
  for (size_t i = 0; i < 4; ++i)
    b[off + i] = static_cast<u8>(v >> (24 - 8 * i));
}
void WriteBE64(std::vector<u8>& b, size_t off, u64 v)
{
  for (size_t i = 0; i < 8; ++i)
    b[off + i] = static_cast<u8>(v >> (56 - 8 * i));
}

// Encrypt the (plaintext) title key with the common key. IV = title_id big-endian
// in the high 8 bytes (the inverse of ESCore::InitTitleImportKey).
std::array<u8, 16> EncryptTitleKey(const std::array<u8, 16>& plain, u64 title_id)
{
  std::array<u8, 16> iv{};
  for (size_t i = 0; i < 8; ++i)
    iv[i] = static_cast<u8>(title_id >> (56 - 8 * i));
  const auto ctx = Common::AES::CreateContextEncrypt(RETAIL_COMMON_KEY.data());
  std::array<u8, 16> out{};
  ctx->Crypt(iv.data(), plain.data(), out.data(), out.size());
  return out;
}

// AES-128-CBC encrypt content with the (plaintext) title key. IV derives from the
// content index. Input is zero-padded to a 16-byte multiple; ES decrypts the whole
// buffer but only hashes/writes the unpadded `size` bytes.
std::vector<u8> EncryptContent(const std::vector<u8>& plain, u16 index,
                               const std::array<u8, 16>& title_key)
{
  const size_t padded = Common::AlignUp(plain.size(), size_t{16});
  std::vector<u8> in(padded, 0);
  std::copy(plain.begin(), plain.end(), in.begin());
  std::array<u8, 16> iv{};
  iv[0] = static_cast<u8>(index >> 8);
  iv[1] = static_cast<u8>(index & 0xFF);
  const auto ctx = Common::AES::CreateContextEncrypt(title_key.data());
  std::vector<u8> out(padded, 0);
  ctx->Crypt(iv.data(), in.data(), out.data(), padded);
  return out;
}

// IMET banner layout (offsets within an opening.bnr / channel banner):
//   0x040  "IMET" magic
//   0x05C  title block: 10 languages x 42 UTF-16BE chars = 0x348 bytes
//   0x5F0  IMET MD5 (16 bytes), computed over [0, 0x600) with this field zeroed
//   0x600  U8 archive (icon.bin / banner.bin / sound.bin) -- the asset payload
constexpr size_t IMET_HEADER_SIZE = 0x600;
constexpr size_t IMET_TITLES_OFFSET = 0x5C;
constexpr size_t IMET_TITLES_SIZE = 0x348;  // 10 langs * 42 * sizeof(char16_t)
constexpr size_t IMET_MD5_OFFSET = 0x5F0;

// A known-good "donor" channel banner (captured from a menu-safe game). We reuse its asset
// payload (icon/banner/sound) and only swap in per-game art + title text, so a quirky disc
// banner can never crash the System Menu's grid renderer. The donor is load-bearing: without
// it, no safe banner can be built and games are skipped (never rendered verbatim). It is
// loaded lazily from the user dir and can be populated by EnsureDonorBanner (auto-capture).
std::optional<std::vector<u8>> s_donor_banner;
std::mutex s_donor_mutex;
bool s_donor_loaded = false;

std::string DonorBannerPath()
{
  return File::GetUserPath(D_USER_IDX) + "forwarder_donor.bnr";
}

std::optional<std::vector<u8>> ReadDonorBannerFile()
{
  std::ifstream file(DonorBannerPath(), std::ios::binary | std::ios::ate);
  if (!file)
    return std::nullopt;
  const std::streamoff size = file.tellg();
  file.seekg(0);
  std::vector<u8> data(static_cast<size_t>(size > 0 ? size : 0));
  if (size < static_cast<std::streamoff>(IMET_HEADER_SIZE + 4) ||
      !file.read(reinterpret_cast<char*>(data.data()), size) || data[0x40] != 'I' ||
      data[0x41] != 'M' || data[0x42] != 'E' || data[0x43] != 'T')
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder: donor banner at {} is missing/invalid", DonorBannerPath());
    return std::nullopt;
  }
  return data;
}

const std::vector<u8>* GetDonorBanner()
{
  std::lock_guard<std::mutex> lock(s_donor_mutex);
  if (!s_donor_loaded)
  {
    s_donor_loaded = true;
    s_donor_banner = ReadDonorBannerFile();
  }
  return s_donor_banner ? &*s_donor_banner : nullptr;
}

// Games to show a yellow "image not loaded" caution tile for, instead of their real banner. The
// set is a small built-in seed of confirmed Wii-Menu-crashers -- Mario Party 9, all regions, the
// one game known to brick the channel grid -- plus the user-editable forwarder_blocklist.txt (one
// game id per line, '#' = comment). If some OTHER game ever crashes the menu, add its id to that
// file to give it a caution tile too; delete the file to clear your additions.
//
// Cached, and invalidated by InvalidateBannerBlocklist() at the start of each sync so an edit to
// the file takes effect without a relaunch. The mutex guards cross-thread access (the forwarder
// sync reads this from a worker thread).
std::mutex s_blocklist_mutex;
std::optional<std::set<std::string>> s_blocklist_cache;

bool IsBannerBlocklisted(const std::string& game_id)
{
  std::lock_guard<std::mutex> lock(s_blocklist_mutex);
  if (!s_blocklist_cache)
  {
    // Mario Party 9 (USA/Asia, Europe, Japan, Korea) -- the confirmed channel-grid crasher.
    std::set<std::string> set{"SSQE01", "SSQP01", "SSQJ01", "SSQK01"};
    std::ifstream file(File::GetUserPath(D_USER_IDX) + "forwarder_blocklist.txt");
    for (std::string line; std::getline(file, line);)
    {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      if (!line.empty() && line.front() != '#')
        set.insert(line);
    }
    s_blocklist_cache = std::move(set);
  }
  return s_blocklist_cache->count(game_id) != 0;
}

// Drop the cached blocklist so the next IsBannerBlocklisted re-reads forwarder_blocklist.txt.
void InvalidateBannerBlocklist()
{
  std::lock_guard<std::mutex> lock(s_blocklist_mutex);
  s_blocklist_cache.reset();
}

// Read the disc's own opening.bnr verbatim (full per-game art: real icon + animation).
// nullopt if there's no readable, structurally-valid IMET banner.
std::optional<std::vector<u8>> ReadFullBanner(const DiscIO::Volume& volume,
                                              const DiscIO::Partition& partition)
{
  const DiscIO::FileSystem* const fs = volume.GetFileSystem(partition);
  if (!fs)
    return std::nullopt;
  const std::unique_ptr<DiscIO::FileInfo> info = fs->FindFileInfo("opening.bnr");
  if (!info)
    return std::nullopt;
  std::vector<u8> banner(info->GetSize());
  if (DiscIO::ReadFile(volume, partition, info.get(), banner.data(), banner.size()) !=
      banner.size())
    return std::nullopt;
  if (banner.size() < IMET_HEADER_SIZE + 4 || banner[0x40] != 'I' || banner[0x41] != 'M' ||
      banner[0x42] != 'E' || banner[0x43] != 'T')
    return std::nullopt;
  return banner;
}

// Build a guaranteed-System-Menu-safe channel banner for a disc: start from the
// known-good donor, patch in this game's IMET titles (inert UTF-16BE text), recompute
// the IMET MD5. Returns nullopt (skip the game) if the donor is missing or the disc
// has no usable opening.bnr title block.
std::optional<std::vector<u8>> BuildSafeBanner(const DiscIO::Volume& volume,
                                               const DiscIO::Partition& partition)
{
  const std::vector<u8>* const donor = GetDonorBanner();
  if (!donor)
    return std::nullopt;
  std::vector<u8> banner = *donor;

  // Copy this game's title block (all language slots) from its own opening.bnr.
  std::array<u8, IMET_TITLES_SIZE> titles{};
  if (DiscIO::ReadFile(volume, partition, "opening.bnr", titles.data(), titles.size(),
                       IMET_TITLES_OFFSET) != titles.size())
    return std::nullopt;
  std::copy(titles.begin(), titles.end(), banner.begin() + IMET_TITLES_OFFSET);

  // Recompute the IMET MD5 over [0, 0x600) with the 16-byte MD5 field zeroed.
  std::fill_n(banner.begin() + IMET_MD5_OFFSET, 16, u8{0});
  std::array<u8, 16> digest{};
  mbedtls_md5_ret(banner.data(), IMET_HEADER_SIZE, digest.data());
  std::copy(digest.begin(), digest.end(), banner.begin() + IMET_MD5_OFFSET);

  // Backstop: confirm the IMET + U8 magics are intact before trusting it.
  if (banner[0x40] != 'I' || banner[0x41] != 'M' || banner[0x42] != 'E' || banner[0x43] != 'T' ||
      banner[0x600] != 0x55 || banner[0x601] != 0xAA || banner[0x602] != 0x38 ||
      banner[0x603] != 0x2D)
    return std::nullopt;

  return banner;
}

// Games whose opening.bnr is known to render safely as a channel banner, used to bootstrap
// the donor scene. (Mario Kart Wii, Wii Sports / Resort, New Super Mario Bros. Wii.)
const std::set<std::string> KNOWN_SAFE_DONOR_GAMES = {
    "RMCE01", "RMCP01", "RMCJ01", "RMCK01",            // Mario Kart Wii
    "RZTE01", "RZTP01", "RZTJ01", "RZTK01", "RZTW01",  // Wii Sports Resort
    "RSPE01", "RSPP01", "RSPJ01", "RSPK01",            // Wii Sports
    "SMNE01", "SMNP01", "SMNJ01", "SMNK01", "SMNW01",  // New Super Mario Bros. Wii
};

// If no donor banner exists yet, capture one from a known-safe game in the library and
// populate the cache. This bootstraps the crash-proof banner pipeline on a fresh install
// using the user's own disc data locally -- no copyrighted art is bundled or redistributed.
// Without a donor, InstallForwarder skips games (never renders a banner verbatim), so this
// is what makes tiles appear at all on first run.
void EnsureDonorBanner(const std::vector<std::string>& disc_paths)
{
  if (GetDonorBanner())
    return;  // already have a valid donor
  for (const std::string& path : disc_paths)
  {
    const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
    if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
      continue;
    const DiscIO::Partition partition = volume->GetGamePartition();
    if (!KNOWN_SAFE_DONOR_GAMES.count(volume->GetGameID(partition)))
      continue;
    const std::optional<std::vector<u8>> bnr = ReadFullBanner(*volume, partition);
    if (!bnr)
      continue;
    {
      std::ofstream out(DonorBannerPath(), std::ios::binary | std::ios::trunc);
      if (!out)
        return;
      out.write(reinterpret_cast<const char*>(bnr->data()),
                static_cast<std::streamsize>(bnr->size()));
    }
    {
      std::lock_guard<std::mutex> lock(s_donor_mutex);  // populate cache so it's seen now
      s_donor_banner = bnr;
      s_donor_loaded = true;
    }
    INFO_LOG_FMT(IOS_ES, "Forwarder: captured donor banner from {}",
                 volume->GetGameID(partition));
    return;
  }
  WARN_LOG_FMT(IOS_ES, "Forwarder: no known-safe game in the library to capture a donor "
                       "banner from; tiles are skipped until a donor exists");
}

std::vector<u8> BuildForwarderTicket(u64 title_id, const std::array<u8, 16>& enc_title_key)
{
  std::vector<u8> t(0x2A4, 0);
  WriteBE32(t, 0x000, 0x00010001);  // signature type: RSA-2048
  static constexpr char ISSUER[] = "Root-CA00000001-XS00000003";
  std::memcpy(&t[0x140], ISSUER, sizeof(ISSUER) - 1);  // null terminator already zero
  // version (0x1BC) = 0 -> V0 ticket
  std::memcpy(&t[0x1BF], enc_title_key.data(), enc_title_key.size());
  WriteBE64(t, 0x1D0, 1);  // ticket_id (nonzero)
  // device_id (0x1D8) = 0
  WriteBE64(t, 0x1DC, title_id);
  // common_key_index (0x1F1) = 0 (retail)
  return t;
}

std::vector<u8> BuildForwarderTMD(u64 title_id, u64 ios_id, u32 content_id, u64 content_size,
                                  const Common::SHA1::Digest& content_sha1)
{
  std::vector<u8> tmd(0x1E4 + 0x24, 0);  // header + one content
  WriteBE32(tmd, 0x000, 0x00010001);     // signature type: RSA-2048
  static constexpr char ISSUER[] = "Root-CA00000001-CP00000004";
  std::memcpy(&tmd[0x140], ISSUER, sizeof(ISSUER) - 1);
  WriteBE64(tmd, 0x184, ios_id);
  WriteBE64(tmd, 0x18C, title_id);
  WriteBE32(tmd, 0x194, 0x00000001);  // title_flags: TITLE_TYPE_DEFAULT
  WriteBE16(tmd, 0x1DC, 1);           // title_version
  WriteBE16(tmd, 0x1DE, 1);           // num_contents
  WriteBE16(tmd, 0x1E0, 0);           // boot_index
  // content record @ 0x1E4 (0x24 bytes)
  WriteBE32(tmd, 0x1E4 + 0x00, content_id);
  WriteBE16(tmd, 0x1E4 + 0x04, 0);       // index
  WriteBE16(tmd, 0x1E4 + 0x06, 0x0001);  // type: normal (not shared/optional)
  WriteBE64(tmd, 0x1E4 + 0x08, content_size);
  std::memcpy(&tmd[0x1E4 + 0x10], content_sha1.data(), content_sha1.size());
  return tmd;
}

// Rewrite the System Menu's channel grid (iplsave.bin) so forwarder tiles stack evenly:
// system channels first (in their existing order), then our forwarders in a stable order by
// game id, packed into consecutive slots with no gaps. The Wii Menu's own reconcile is what
// leaves a hole when a game is removed and scatters newly-added tiles; pre-packing the file
// (after the NAND set is already reconciled) fixes both. SAFE: the System Menu validates this
// file's MD5 on boot and regenerates a default layout if it's wrong, so a bad write can never
// brick the menu (worst case it falls back to its own ordering).
//
// iplsave.bin (decompiled from the retail System Menu, koopthekoopa/wii-ipl): magic "RIPL",
// 0x4C0 bytes (version 3), all big-endian. chanInfo[48] @0x10 (16-byte SInfo: u8 primaryType,
// u8 secondaryType, u8 reserved[2], be32 sceneID, be32 titleType, be32 titleCode), titleCache
// [48] @0x320 (be64 title id each), MD5 @0x4B0 over bytes [0, 0x4B0).
void RepackWiiMenuChannelLayout(const std::vector<ForwarderMapEntry>& desired)
{
  constexpr size_t FILE_SIZE = 0x4C0, CHAN_OFF = 0x10, TCACHE_OFF = 0x320, MD5_OFF = 0x4B0;
  constexpr size_t SLOTS = 48, INFO = 16;
  const std::string path =
      Common::GetTitleDataPath(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured) +
      "/iplsave.bin";

  std::vector<u8> d(FILE_SIZE);
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return;  // the menu hasn't created it yet (first run) -> it'll exist next launch
    in.read(reinterpret_cast<char*>(d.data()), FILE_SIZE);
    if (in.gcount() != static_cast<std::streamsize>(FILE_SIZE))
      return;
  }
  const auto rd32 = [&](size_t o) {
    return (u32{d[o]} << 24) | (u32{d[o + 1]} << 16) | (u32{d[o + 2]} << 8) | u32{d[o + 3]};
  };
  // Only touch a known version-3 RIPL file (conservative: skip anything unexpected).
  if (std::memcmp(d.data(), "RIPL", 4) != 0 || rd32(0x04) != FILE_SIZE || rd32(0x08) != 3)
    return;

  // Installed forwarders: low32 title code -> game id (for validity + stable ordering).
  std::map<u32, std::string> fwd;
  for (const ForwarderMapEntry& e : desired)
    if (IsForwarderTitle(e.title_id))
      fwd.emplace(static_cast<u32>(e.title_id & 0xFFFFFFFFu), e.game_id);

  // Snapshot the original bytes so we can skip the write (and avoid churning a brick-sensitive
  // NAND file) when the repacked layout is byte-identical to what's already on disk.
  const std::vector<u8> original = d;

  // Walk the current grid: keep non-forwarder channels (Disc/Mii/WiiWare/...) in order;
  // collect installed forwarders (dropping any stale tile whose game is no longer present).
  std::vector<std::array<u8, INFO>> others;
  std::vector<u32> forwarders;
  std::set<u32> seen;
  for (size_t s = 0; s < SLOTS; ++s)
  {
    const size_t o = CHAN_OFF + s * INFO;
    if (d[o] == 0)  // PRIMARY_TYPE_NONE -> empty slot
      continue;
    const u32 ttype = rd32(o + 8), tcode = rd32(o + 12);
    if (ttype == 0x00010001u && (tcode >> 24) == 0x46)  // our forwarder namespace
    {
      if (fwd.count(tcode) && seen.insert(tcode).second)
        forwarders.push_back(tcode);
    }
    else
    {
      std::array<u8, INFO> info;
      std::copy(d.begin() + o, d.begin() + o + INFO, info.begin());
      others.push_back(info);
    }
  }
  // Add installed forwarders not yet in the grid (newly synced this run).
  for (const auto& [tcode, game_id] : fwd)
    if (seen.insert(tcode).second)
      forwarders.push_back(tcode);
  // Deterministic order so a given game always lands in the same place.
  std::sort(forwarders.begin(), forwarders.end(),
            [&](u32 a, u32 b) { return fwd[a] < fwd[b]; });

  // Repack: clear the grid, then lay out [system channels...][forwarders...], no gaps.
  std::fill(d.begin() + CHAN_OFF, d.begin() + CHAN_OFF + SLOTS * INFO, u8{0});
  std::fill(d.begin() + TCACHE_OFF, d.begin() + TCACHE_OFF + SLOTS * 8, u8{0});
  size_t slot = 0;
  for (const std::array<u8, INFO>& info : others)
  {
    if (slot >= SLOTS)
      break;
    const size_t o = CHAN_OFF + slot * INFO;
    std::copy(info.begin(), info.end(), d.begin() + o);
    const u32 ttype = (u32{info[8]} << 24) | (u32{info[9]} << 16) | (u32{info[10]} << 8) | info[11];
    const u32 tcode =
        (u32{info[12]} << 24) | (u32{info[13]} << 16) | (u32{info[14]} << 8) | info[15];
    WriteBE64(d, TCACHE_OFF + slot * 8, (u64{ttype} << 32) | tcode);
    ++slot;
  }
  for (const u32 tcode : forwarders)
  {
    if (slot >= SLOTS)
    {
      WARN_LOG_FMT(IOS_ES, "Wii Menu has only {} channel slots; some forwarder tiles omitted",
                   SLOTS);
      break;
    }
    const size_t o = CHAN_OFF + slot * INFO;
    d[o + 0] = 3;  // PRIMARY_TYPE_CHANNEL
    d[o + 1] = 0;  // SECONARY_TYPE_NORMAL
    WriteBE32(d, o + 4, 0x0000000Eu);  // sceneID = SCENE_ID_WAD_CHANNEL
    WriteBE32(d, o + 8, 0x00010001u);  // titleType
    WriteBE32(d, o + 12, tcode);       // titleCode
    WriteBE64(d, TCACHE_OFF + slot * 8, (0x00010001ULL << 32) | tcode);
    ++slot;
  }

  // Recompute the MD5 over [0, 0x4B0) and write the file back -- unless nothing changed.
  std::array<u8, 16> digest{};
  mbedtls_md5_ret(d.data(), MD5_OFF, digest.data());
  std::copy(digest.begin(), digest.end(), d.begin() + MD5_OFF);
  if (d == original)
    return;  // already packed correctly; skip the redundant write
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return;
  out.write(reinterpret_cast<const char*>(d.data()), FILE_SIZE);
  INFO_LOG_FMT(IOS_ES, "Wii Menu layout repacked: {} system + {} forwarder tiles, no gaps",
               others.size(), forwarders.size());
}
}  // namespace

void SetForwarderBootHandler(std::function<void(const std::string&)> handler)
{
  s_forwarder_boot_handler = std::move(handler);
}

void RequestForwarderBoot(const std::string& disc_path)
{
  if (s_forwarder_boot_handler)
    s_forwarder_boot_handler(disc_path);
  else
    ERROR_LOG_FMT(IOS_ES, "RequestForwarderBoot: no handler registered (disc: {})", disc_path);
}

void SetSafeBannerMode(bool enabled)
{
  s_safe_banner_mode.store(enabled);
}
bool IsSafeBannerMode()
{
  return s_safe_banner_mode.load();
}
void SetWiiMenuBootPending(bool pending)
{
  // Reset the once-per-boot brick guard when arming a fresh menu boot.
  if (pending)
    s_brick_handled_this_boot.store(false);
  s_wii_menu_boot_pending.store(pending);
}

bool NotePanicMessageMaybeBrick(const char* text)
{
  if (!s_wii_menu_boot_pending.load() || text == nullptr)
    return false;
  const std::string_view msg(text);
  // Memory-access panics carry "PC = 0x..."; match that (plus the English wording) so we catch the
  // brick regardless of UI translation. Gated on the menu-boot window above, so a normal game's
  // stray fault is never misattributed.
  if (msg.find("PC = 0x") == std::string_view::npos &&
      msg.find("Invalid read") == std::string_view::npos &&
      msg.find("Invalid write") == std::string_view::npos)
  {
    return false;
  }
  // A bad banner can fault repeatedly (a garbage-pointer loop spewing many panics). Act on the
  // FIRST one only; suppress the rest (return true without re-processing) so the flood neither
  // spams dialogs nor re-runs the heal. Reset per boot in SetWiiMenuBootPending.
  if (s_brick_handled_this_boot.exchange(true))
    return true;
  // We can't reliably tell WHICH banner crashed -- the System Menu reads every banner before it
  // renders them, so "the last one read" is not the culprit (guessing wrongly quarantined an
  // innocent game). So don't guess: switch to blanket safe-banner mode (the next launch rebuilds
  // every tile as a safe placeholder, so the menu boots cleanly) and request that one-shot
  // rebuild. The user pins the offending game by adding its id to forwarder_blocklist.txt -- it
  // then gets a caution tile while every other game keeps its real banner.
  const std::string dir = File::GetUserPath(D_USER_IDX);
  File::CreateEmptyFile(dir + SAFE_MODE_MARKER);
  File::CreateEmptyFile(dir + REGEN_PENDING_MARKER);
  s_safe_banner_mode.store(true);
  s_wii_menu_brick_detected.store(true);
  WARN_LOG_FMT(IOS_ES, "Wii Menu banner brick detected; engaging safe-banner mode for relaunch");
  return true;
}

bool ConsumeWiiMenuBrickDetected()
{
  return s_wii_menu_brick_detected.exchange(false);
}

bool HasSafeBannerMarker()
{
  return File::Exists(File::GetUserPath(D_USER_IDX) + SAFE_MODE_MARKER);
}

bool ConsumeBannerRegenPending()
{
  const std::string path = File::GetUserPath(D_USER_IDX) + REGEN_PENDING_MARKER;
  if (!File::Exists(path))
    return false;
  File::Delete(path);
  return true;
}

u64 ComputeForwarderTitleId(const std::string& game_id, u16 revision)
{
  // Deterministic, file-location-independent id from the disc's stable identity.
  // Channel namespace (0x00010001) with reserved low byte 'F' (0x46) so it never
  // collides with real Nintendo channels (whose low32 starts with 'H' / 0x48).
  const std::string key = game_id + "/" + std::to_string(revision);
  const Common::SHA1::Digest d =
      Common::SHA1::CalculateDigest(reinterpret_cast<const u8*>(key.data()), key.size());
  const u32 hash24 = (u32{d[0]} << 16) | (u32{d[1]} << 8) | u32{d[2]};
  const u32 low32 = 0x46000000u | hash24;
  return (0x00010001ULL << 32) | low32;
}

bool IsForwarderTitle(u64 title_id)
{
  return (title_id >> 32) == 0x00010001ULL && ((title_id >> 24) & 0xFF) == 0x46;
}

std::optional<std::string> LookupForwarderDiscPath(u64 title_id)
{
  std::lock_guard<std::mutex> lock(s_forwarder_map_mutex);
  if (!s_forwarder_map_loaded)
  {
    // Mark loaded even on failure so we don't re-hit the disk on every launch. Reuse the
    // single full parser (which guards title_id parsing) and project to title_id -> path.
    s_forwarder_map_loaded = true;
    for (const ForwarderMapEntry& e : LoadForwarderMapFull())
      s_forwarder_map.emplace(e.title_id, e.disc_path);
  }
  const auto it = s_forwarder_map.find(title_id);
  if (it == s_forwarder_map.end())
    return std::nullopt;
  return it->second;
}

void ReloadForwarderMap()
{
  std::lock_guard<std::mutex> lock(s_forwarder_map_mutex);
  s_forwarder_map.clear();
  s_forwarder_map_loaded = false;
}

std::optional<ForwarderInfo> InstallForwarder(const std::string& disc_path)
{
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(disc_path);
  if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder: '{}' is not a Wii disc; skipping", disc_path);
    return std::nullopt;
  }
  const DiscIO::Partition partition = volume->GetGamePartition();
  const std::string game_id = volume->GetGameID(partition);
  const u16 revision = volume->GetRevision(partition).value_or(0);
  if (game_id.empty())
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder: '{}' has no game id; skipping", disc_path);
    return std::nullopt;
  }

  // Build the channel banner:
  //  * a normal game gets its OWN real banner verbatim -> truest per-game art. If that banner
  //    crashes the System Menu, the self-heal attributes the crash to this game, quarantines it,
  //    and it becomes a caution tile on the next launch ("crash once to auto-find the bad game").
  //  * a flagged game (manual blocklist or runtime auto-quarantine) gets the yellow "image not
  //    loaded" caution tile, built on the proven-safe donor scene so it can't brick.
  //  * with no readable game banner, or while in blanket safe-banner mode (the loop backstop),
  //    falls back to the plain donor tile. With no donor at all the game is skipped (no tile).
  static const bool s_banner_tools_ok = Banner::RunSelfTests();
  const std::vector<u8>* const donor = GetDonorBanner();
  const std::optional<std::vector<u8>> game_bnr = ReadFullBanner(*volume, partition);
  const bool blocklisted = IsBannerBlocklisted(game_id);
  std::optional<std::vector<u8>> banner;
  if (game_bnr && !blocklisted && !IsSafeBannerMode())
    banner = *game_bnr;  // the game's own real banner (verbatim); auto-quarantine is the safety net
  if (!banner && blocklisted && donor && s_banner_tools_ok && game_bnr)
    banner = Banner::BuildCautionBanner(*donor, *game_bnr);  // yellow "image not loaded" tile
  if (!banner && donor)
    banner = BuildSafeBanner(*volume, partition);  // plain donor scene + game titles (safe fallback)
  if (!banner)
  {
    WARN_LOG_FMT(IOS_ES,
                 "Forwarder: '{}' ({}) - no usable banner (no game banner / no donor); skipping",
                 disc_path, game_id);
    return std::nullopt;
  }

  const u64 title_id = ComputeForwarderTitleId(game_id, revision);
  const u64 ios_id = 0x0000000100000000ULL | 0x3D;  // never booted (launch is intercepted)
  const u32 content_id = 0;
  const std::array<u8, 16> title_key_plain{};
  const std::vector<u8>& content_plain = *banner;

  const Common::SHA1::Digest content_sha1 =
      Common::SHA1::CalculateDigest(content_plain.data(), content_plain.size());
  const std::vector<u8> enc_content = EncryptContent(content_plain, 0, title_key_plain);
  const std::array<u8, 16> enc_title_key = EncryptTitleKey(title_key_plain, title_id);
  const std::vector<u8> ticket_bytes = BuildForwarderTicket(title_id, enc_title_key);
  const std::vector<u8> tmd_bytes =
      BuildForwarderTMD(title_id, ios_id, content_id, content_plain.size(), content_sha1);

  // Self-check: round-trip our hand-built blobs through Dolphin's own readers.
  {
    const IOS::ES::TicketReader ticket{std::vector<u8>(ticket_bytes)};
    const IOS::ES::TMDReader tmd{std::vector<u8>(tmd_bytes)};
    if (!ticket.IsValid() || !tmd.IsValid() || ticket.GetTitleId() != title_id ||
        tmd.GetTitleId() != title_id)
    {
      ERROR_LOG_FMT(IOS_ES, "Forwarder self-check failed for {} ({:016x})", game_id, title_id);
      return std::nullopt;
    }
  }

  // Install into the configured NAND via the ES import API (fake-signed).
  IOS::HLE::Kernel ios;
  auto& es = ios.GetESCore();
  IOS::HLE::ESCore::Context context;
  const std::vector<u8> no_certs;
  if (es.ImportTicket(ticket_bytes, no_certs, IOS::HLE::ESCore::TicketImportType::Unpersonalised,
                      IOS::HLE::ESCore::VerifySignature::No) < 0 ||
      es.ImportTitleInit(context, tmd_bytes, no_certs, IOS::HLE::ESCore::VerifySignature::No) < 0)
  {
    ERROR_LOG_FMT(IOS_ES, "Forwarder {} ({:016x}): ticket/title init failed", game_id, title_id);
    return std::nullopt;
  }
  if (es.ImportContentBegin(context, title_id, content_id) < 0 ||
      es.ImportContentData(context, 0, enc_content.data(), static_cast<u32>(enc_content.size())) <
          0 ||
      es.ImportContentEnd(context, 0) < 0)
  {
    es.ImportTitleCancel(context);
    ERROR_LOG_FMT(IOS_ES, "Forwarder {} ({:016x}): content import failed", game_id, title_id);
    return std::nullopt;
  }
  if (es.ImportTitleDone(context) < 0)
  {
    es.ImportTitleCancel(context);
    return std::nullopt;
  }

  INFO_LOG_FMT(IOS_ES, "Forwarder installed: {} rev{} -> {:016x}", game_id, revision, title_id);
  return ForwarderInfo{title_id, game_id, revision};
}

bool UninstallForwarder(u64 title_id)
{
  // Never touch anything outside our forwarder namespace.
  if (!IsForwarderTitle(title_id))
    return false;
  IOS::HLE::Kernel ios;
  const auto ret = ios.GetESCore().DeleteTitle(title_id);
  // Best-effort: drop the ticket too so the System Menu tile fully disappears.
  File::Delete(Common::GetTicketFileName(title_id));
  if (ret < 0)
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder uninstall {:016x}: DeleteTitle failed ({})", title_id,
                 static_cast<int>(ret));
    return false;
  }
  INFO_LOG_FMT(IOS_ES, "Forwarder uninstalled: {:016x}", title_id);
  return true;
}

ForwarderSyncResult SyncForwardersWithLibrary(const std::vector<std::string>& current_disc_paths,
                                              bool force_reinstall, bool full_rebuild)
{
  ForwarderSyncResult result;
  if (s_forwarder_sync_running.exchange(true))
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder sync already in progress; skipping");
    return result;
  }
  const Common::ScopeGuard running_guard{[] { s_forwarder_sync_running.store(false); }};

  // Re-read the blocklist from disk so a just-quarantined game (or a user-deleted list) is
  // honored this run, even on an in-session Tools > Sync without a relaunch.
  InvalidateBannerBlocklist();

  // One-time migration to the real-banner model: the first sync after upgrading rebuilds every
  // tile so games installed by older versions as plain-donor switch to their own real banner (and
  // any banner that crashes the menu then goes through the auto-quarantine flow). Runs once.
  const std::string migrated_marker = File::GetUserPath(D_USER_IDX) + "forwarder_realart_migrated";
  if (!File::Exists(migrated_marker))
  {
    full_rebuild = true;
    File::CreateEmptyFile(migrated_marker);
  }

  // A full rebuild -- the one-time migration or an explicit --generate-forwarders -- reinstalls
  // every tile with its real banner, so it also clears any sticky blanket safe-banner state. This
  // is the user's clean "retry real art" path after the crash-loop backstop has engaged (a
  // genuinely unresolvable crash simply re-engages it). The auto-heal (force_reinstall WITHOUT
  // full_rebuild) deliberately does NOT clear it, so the backstop stays put and can't oscillate.
  if (full_rebuild)
  {
    File::Delete(File::GetUserPath(D_USER_IDX) + SAFE_MODE_MARKER);
    s_safe_banner_mode.store(false);
  }

  // Phase 0: make sure a donor banner exists (capture one from a known-safe game if not), so
  // the crash-proof banner pipeline has a host scene. Without it, games are skipped, not bricked.
  EnsureDonorBanner(current_disc_paths);

  // Phase 1: the existing map is the authoritative "installed" set.
  const std::vector<ForwarderMapEntry> old_entries = LoadForwarderMapFull();
  std::map<std::string, ForwarderMapEntry> old_by_path;
  std::map<u64, ForwarderMapEntry> old_by_tid;
  for (const ForwarderMapEntry& e : old_entries)
  {
    old_by_path.emplace(e.disc_path, e);
    old_by_tid.emplace(e.title_id, e);
  }

  // Force regen. Two modes:
  //  * Blanket (full_rebuild -- the one-time migration or an explicit --generate-forwarders, OR
  //    safe-banner mode): drop EVERY forwarder and rebuild them all. Banner type then follows the
  //    mode: real verbatim banners normally, or plain safe donors while in safe-banner mode.
  //  * Selective (the post-brick heal): drop ONLY the quarantined games so they're rebuilt as
  //    caution tiles, while every other game keeps its existing real banner untouched (its path
  //    stays in old_by_path -> kept as-is below). This is the common heal path.
  if (force_reinstall || full_rebuild)
  {
    const bool blanket = full_rebuild || IsSafeBannerMode();
    IOS::HLE::Kernel ios;
    for (const u64 tid : ios.GetESCore().GetInstalledTitles())
    {
      if (!IsForwarderTitle(tid))
        continue;
      const auto e = old_by_tid.find(tid);
      const bool quarantined = e != old_by_tid.end() && IsBannerBlocklisted(e->second.game_id);
      if (blanket || quarantined)
        UninstallForwarder(tid);
    }
    if (blanket)
    {
      old_by_path.clear();
      old_by_tid.clear();
    }
    else
    {
      // Drop only the quarantined entries from the "keep" maps so they take the reinstall path.
      for (auto it = old_by_path.begin(); it != old_by_path.end();)
        it = IsBannerBlocklisted(it->second.game_id) ? old_by_path.erase(it) : std::next(it);
      for (auto it = old_by_tid.begin(); it != old_by_tid.end();)
        it = IsBannerBlocklisted(it->second.game_id) ? old_by_tid.erase(it) : std::next(it);
    }
  }

  // Phases 2-3: classify each current library path into the desired set.
  std::vector<ForwarderMapEntry> desired;
  std::unordered_set<u64> desired_tids;
  for (const std::string& path : current_disc_paths)
  {
    // Already installed at this exact path -> keep as-is (no disc open, no import).
    const auto present = old_by_path.find(path);
    if (present != old_by_path.end())
    {
      if (desired_tids.insert(present->second.title_id).second)
      {
        desired.push_back(present->second);
        ++result.already_present;
      }
      continue;
    }
    // New path: identify the disc to compute its deterministic forwarder id.
    const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
    if (!volume || volume->GetVolumeType() != DiscIO::Platform::WiiDisc)
      continue;  // not a Wii disc -> ignore (do NOT treat as a removal)
    const DiscIO::Partition partition = volume->GetGamePartition();
    const std::string game_id = volume->GetGameID(partition);
    if (game_id.empty())
      continue;
    const u16 revision = volume->GetRevision(partition).value_or(0);
    const u64 tid = ComputeForwarderTitleId(game_id, revision);
    if (desired_tids.count(tid))
      continue;  // duplicate dump of a game already handled this run

    const auto moved = old_by_tid.find(tid);
    if (moved != old_by_tid.end())
    {
      // Moved/renamed file, same game identity -> already installed, just repoint.
      ForwarderMapEntry e = moved->second;
      e.disc_path = path;
      desired.push_back(e);
      desired_tids.insert(tid);
      ++result.moved;
      continue;
    }
    // Not in the map. If the title already exists in NAND (e.g. the map was lost and is
    // being rebuilt), just re-record it; otherwise build + import it (the expensive path).
    if (File::Exists(Common::GetTMDFileName(tid)))
    {
      desired.push_back({tid, game_id, revision, path});
      desired_tids.insert(tid);
      ++result.already_present;
      continue;
    }
    const std::optional<ForwarderInfo> info = InstallForwarder(path);
    if (!info)
    {
      ++result.failed;
      continue;
    }
    desired.push_back({info->title_id, info->game_id, info->revision, path});
    desired_tids.insert(info->title_id);
    ++result.installed;
  }

  // Phase 4: remove NAND forwarder titles no longer wanted (removed games + orphans).
  // desired_tids is fully computed first, so a moved game (its id stays desired) is never deleted.
  std::vector<u64> installed_titles;
  {
    IOS::HLE::Kernel ios;
    installed_titles = ios.GetESCore().GetInstalledTitles();
  }
  for (const u64 tid : installed_titles)
  {
    if (!IsForwarderTitle(tid) || desired_tids.count(tid))
      continue;
    if (UninstallForwarder(tid))
      ++result.uninstalled;
    else
      ++result.orphaned;
  }

  // Phase 5: persist only if the (title_id, disc_path) set actually changed.
  const auto key = [](const ForwarderMapEntry& e) {
    return fmt::format("{:016x}|{}", e.title_id, e.disc_path);
  };
  std::set<std::string> old_keys, new_keys;
  for (const ForwarderMapEntry& e : old_entries)
    old_keys.insert(key(e));
  for (const ForwarderMapEntry& e : desired)
    new_keys.insert(key(e));
  if (old_keys != new_keys && WriteForwarderMap(desired))
  {
    ReloadForwarderMap();
    result.map_modified = true;
  }

  // Pack the forwarder tiles into the Wii Menu's channel grid so they stack evenly (no gaps
  // when a game is removed, deterministic order instead of scattered). Runs every sync; the
  // menu safely falls back to its own layout if the file is ever malformed.
  RepackWiiMenuChannelLayout(desired);

  INFO_LOG_FMT(IOS_ES,
               "Forwarder sync: installed={} moved={} present={} uninstalled={} failed={} (map {})",
               result.installed, result.moved, result.already_present, result.uninstalled,
               result.failed, result.map_modified ? "updated" : "unchanged");
  return result;
}

static bool ImportWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad,
                      IOS::HLE::ESCore::VerifySignature verify_signature)
{
  if (!wad.GetTicket().IsValid() || !wad.GetTMD().IsValid())
  {
    PanicAlertFmtT("WAD installation failed: The selected file is not a valid WAD.");
    return false;
  }

  const auto tmd = wad.GetTMD();
  auto& es = ios.GetESCore();
  const auto fs = ios.GetFS();

  IOS::HLE::ESCore::Context context;
  IOS::HLE::ReturnCode ret;

  // Ensure the common key index is correct, as it's checked by IOS.
  IOS::ES::TicketReader ticket = wad.GetTicketWithFixedCommonKey();

  while ((ret = es.ImportTicket(ticket.GetBytes(), wad.GetCertificateChain(),
                                IOS::HLE::ESCore::TicketImportType::Unpersonalised,
                                verify_signature)) < 0 ||
         (ret = es.ImportTitleInit(context, tmd.GetBytes(), wad.GetCertificateChain(),
                                   verify_signature)) < 0)
  {
    if (ret != IOS::HLE::IOSC_FAIL_CHECKVALUE)
    {
      PanicAlertFmtT("WAD installation failed: Could not initialise title import (error {0}).",
                     std::to_underlying(ret));
    }
    return false;
  }

  const bool contents_imported = [&] {
    const u64 title_id = tmd.GetTitleId();
    for (const IOS::ES::Content& content : tmd.GetContents())
    {
      const std::vector<u8> data = wad.GetContent(content.index);

      if (es.ImportContentBegin(context, title_id, content.id) < 0 ||
          es.ImportContentData(context, 0, data.data(), static_cast<u32>(data.size())) < 0 ||
          es.ImportContentEnd(context, 0) < 0)
      {
        PanicAlertFmtT("WAD installation failed: Could not import content {0:08x}.", content.id);
        return false;
      }
    }
    return true;
  }();

  if ((contents_imported && es.ImportTitleDone(context) < 0) ||
      (!contents_imported && es.ImportTitleCancel(context) < 0))
  {
    PanicAlertFmtT("WAD installation failed: Could not finalise title import.");
    return false;
  }

  // Under normal conditions, these two log files are created by the Wii Shop channel at some point
  // during the process of downloading a game, and some games (eg. Mega Man 9) refuse to load DLC if
  // they are not present. So ensure they exist and create them if they don't.
  const bool shop_logs_exist = [&] {
    const std::array<u8, 32> dummy_data{};
    for (const std::string path : {"/shared2/ec/shopsetu.log", "/shared2/succession/shop.log"})
    {
      constexpr IOS::HLE::FS::Mode rw_mode = IOS::HLE::FS::Mode::ReadWrite;
      if (fs->CreateFullPath(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, 0,
                             {rw_mode, rw_mode, rw_mode}) != IOS::HLE::FS::ResultCode::Success)
      {
        return false;
      }

      const auto old_handle = fs->OpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, rw_mode);
      if (old_handle)
        continue;

      const auto new_handle = fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path,
                                                    {rw_mode, rw_mode, rw_mode});
      if (!new_handle || !new_handle->Write(dummy_data.data(), dummy_data.size()))
        return false;
    }
    return true;
  }();

  if (!shop_logs_exist)
  {
    PanicAlertFmtT("WAD installation failed: Could not create Wii Shop log files.");
    return false;
  }

  return true;
}

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType install_type)
{
  if (!wad.GetTMD().IsValid())
    return false;

  SysConf sysconf{ios.GetFS()};
  SysConf::Entry* tid_entry = sysconf.GetOrAddEntry("IPL.TID", SysConf::Entry::Type::LongLong);
  const u64 previous_temporary_title_id = Common::swap64(tid_entry->GetData<u64>(0));
  const u64 title_id = wad.GetTMD().GetTitleId();

  // Skip the install if the WAD is already installed.
  const auto installed_contents = ios.GetESCore().GetStoredContentsFromTMD(
      wad.GetTMD(), IOS::HLE::ESCore::CheckContentHashes::Yes);
  if (wad.GetTMD().GetContents() == installed_contents)
  {
    // Clear the "temporary title ID" flag in case the user tries to permanently install a title
    // that has already been imported as a temporary title.
    if (previous_temporary_title_id == title_id && install_type == InstallType::Permanent)
      tid_entry->SetData<u64>(0);
    return true;
  }

  // If a different version is currently installed, warn the user to make sure
  // they don't overwrite the current version by mistake.
  const IOS::ES::TMDReader installed_tmd = ios.GetESCore().FindInstalledTMD(title_id);
  const bool has_another_version =
      installed_tmd.IsValid() && installed_tmd.GetTitleVersion() != wad.GetTMD().GetTitleVersion();
  if (has_another_version &&
      !AskYesNoFmtT("A different version of this title is already installed on the NAND.\n\n"
                    "Installed version: {0}\nWAD version: {1}\n\n"
                    "Installing this WAD will replace it irreversibly. Continue?",
                    installed_tmd.GetTitleVersion(), wad.GetTMD().GetTitleVersion()))
  {
    return false;
  }

  // Delete a previous temporary title, if it exists.
  if (previous_temporary_title_id)
    ios.GetESCore().DeleteTitleContent(previous_temporary_title_id);

  // A lot of people use fakesigned WADs, so disable signature checking when installing a WAD.
  if (!ImportWAD(ios, wad, IOS::HLE::ESCore::VerifySignature::No))
    return false;

  // Keep track of the title ID so this title can be removed to make room for any future install.
  // We use the same mechanism as the System Menu for temporary SD card title data.
  if (!has_another_version && install_type == InstallType::Temporary)
    tid_entry->SetData<u64>(Common::swap64(title_id));
  else
    tid_entry->SetData<u64>(0);

  return true;
}

bool InstallWAD(const std::string& wad_path)
{
  std::unique_ptr<DiscIO::VolumeWAD> wad = DiscIO::CreateWAD(wad_path);
  if (!wad)
    return false;

  IOS::HLE::Kernel ios;
  return InstallWAD(ios, *wad, InstallType::Permanent);
}

bool UninstallTitle(u64 title_id)
{
  IOS::HLE::Kernel ios;
  return ios.GetESCore().DeleteTitleContent(title_id) == IOS::HLE::IPC_SUCCESS;
}

bool IsTitleInstalled(u64 title_id)
{
  IOS::HLE::Kernel ios;
  const auto entries = ios.GetFS()->ReadDirectory(0, 0, Common::GetTitleContentPath(title_id));

  if (!entries)
    return false;

  // Since this isn't IOS and we only need a simple way to figure out if a title is installed,
  // we make the (reasonable) assumption that having more than just the TMD in the content
  // directory means that the title is installed.
  return std::ranges::any_of(*entries, [](const std::string& file) { return file != "title.tmd"; });
}

bool IsTMDImported(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  const auto entries = fs.ReadDirectory(0, 0, Common::GetTitleContentPath(title_id));
  return entries &&
         std::ranges::any_of(*entries, [](const std::string& file) { return file == "title.tmd"; });
}

IOS::ES::TMDReader FindBackupTMD(IOS::HLE::FS::FileSystem& fs, u64 title_id)
{
  auto file = fs.OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                          "/title/00000001/00000002/data/tmds.sys", IOS::HLE::FS::Mode::Read);
  if (!file)
    return {};

  // structure of this file is as follows:
  // - 32 bytes descriptor of a TMD, which contains a title ID and a length
  // - the TMD, with padding aligning to 32 bytes
  // - repeat for as many TMDs as stored
  while (true)
  {
    std::array<u8, 32> descriptor;
    if (!file->Read(descriptor.data(), descriptor.size()))
      return {};

    const u64 tid = Common::swap64(descriptor.data());
    const u32 tmd_length = Common::swap32(descriptor.data() + 8);
    if (tid == title_id)
    {
      // found the right TMD
      std::vector<u8> tmd_bytes(tmd_length);
      if (!file->Read(tmd_bytes.data(), tmd_length))
        return {};
      return IOS::ES::TMDReader(std::move(tmd_bytes));
    }

    // not the right TMD, skip this one and go to the next
    if (!file->Seek(Common::AlignUp(tmd_length, 32), IOS::HLE::FS::SeekMode::Current))
      return {};
  }
}

bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id)
{
  if (IsTMDImported(fs, title_id))
    return true;

  auto tmd = FindBackupTMD(fs, title_id);
  if (!tmd.IsValid())
    return false;

  IOS::HLE::ESCore::Context context;
  context.uid = IOS::SYSMENU_UID;
  context.gid = IOS::SYSMENU_GID;
  const auto import_result =
      es.ImportTmd(context, tmd.GetBytes(), Titles::SYSTEM_MENU, IOS::ES::TITLE_TYPE_DEFAULT);
  if (import_result != IOS::HLE::IPC_SUCCESS)
    return false;

  return es.ImportTitleDone(context) == IOS::HLE::IPC_SUCCESS;
}

// Common functionality for system updaters.
class SystemUpdater
{
public:
  virtual ~SystemUpdater() = default;

protected:
  struct TitleInfo
  {
    u64 id;
    u16 version;
  };

  std::string GetDeviceRegion();
  std::string GetDeviceId();

  IOS::HLE::Kernel m_ios;
};

std::string SystemUpdater::GetDeviceRegion()
{
  // Try to determine the region from an installed system menu.
  const auto tmd = m_ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
  if (tmd.IsValid())
  {
    const DiscIO::Region region = tmd.GetRegion();
    static const std::map<DiscIO::Region, std::string> regions = {{DiscIO::Region::NTSC_J, "JPN"},
                                                                  {DiscIO::Region::NTSC_U, "USA"},
                                                                  {DiscIO::Region::PAL, "EUR"},
                                                                  {DiscIO::Region::NTSC_K, "KOR"},
                                                                  {DiscIO::Region::Unknown, "EUR"}};
    return regions.at(region);
  }
  return "";
}

std::string SystemUpdater::GetDeviceId()
{
  u32 ios_device_id;
  if (m_ios.GetESCore().GetDeviceId(&ios_device_id) < 0)
    return "";
  return std::to_string((u64(1) << 32) | ios_device_id);
}

class OnlineSystemUpdater final : public SystemUpdater
{
public:
  OnlineSystemUpdater(UpdateCallback update_callback, std::string region);
  UpdateResult DoOnlineUpdate();

private:
  struct Response
  {
    std::string content_prefix_url;
    std::vector<TitleInfo> titles;
  };

  Response GetSystemTitles();
  Response ParseTitlesResponse(std::span<const u8> response) const;
  bool ShouldInstallTitle(const TitleInfo& title);

  UpdateResult InstallTitleFromNUS(const std::string& prefix_url, const TitleInfo& title,
                                   std::unordered_set<u64>* updated_titles);

  // Helper functions to download contents from NUS.
  std::pair<IOS::ES::TMDReader, std::vector<u8>> DownloadTMD(const std::string& prefix_url,
                                                             const TitleInfo& title);
  std::pair<std::vector<u8>, std::vector<u8>> DownloadTicket(const std::string& prefix_url,
                                                             const TitleInfo& title);
  std::optional<std::vector<u8>> DownloadContent(const std::string& prefix_url,
                                                 const TitleInfo& title, u32 cid);

  UpdateCallback m_update_callback;
  std::string m_requested_region;
  Common::HttpRequest m_http{std::chrono::minutes{3}};
};

OnlineSystemUpdater::OnlineSystemUpdater(UpdateCallback update_callback, std::string region)
    : m_update_callback(std::move(update_callback)), m_requested_region(std::move(region))
{
}

OnlineSystemUpdater::Response
OnlineSystemUpdater::ParseTitlesResponse(std::span<const u8> response) const
{
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(response.data(), response.size());
  if (!result)
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Could not parse response");
    return {};
  }

  // pugixml doesn't fully support namespaces and ignores them.
  const pugi::xml_node node = doc.select_node("//GetSystemUpdateResponse").node();
  if (!node)
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Could not find response node");
    return {};
  }

  const int code = node.child("ErrorCode").text().as_int();
  if (code != 0)
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Non-zero error code ({})", code);
    return {};
  }

  // libnup uses the uncached URL, not the cached one. However, that one is way, way too slow,
  // so let's use the cached endpoint.
  Response info;
  info.content_prefix_url = node.child("ContentPrefixURL").text().as_string();
  // Disable HTTPS because we can't use it without a device certificate.
  info.content_prefix_url = ReplaceAll(info.content_prefix_url, "https://", "http://");
  if (info.content_prefix_url.empty())
  {
    ERROR_LOG_FMT(CORE, "ParseTitlesResponse: Empty content prefix URL");
    return {};
  }

  for (const pugi::xml_node& title_node : node.children("TitleVersion"))
  {
    const u64 title_id = std::stoull(title_node.child("TitleId").text().as_string(), nullptr, 16);
    const u16 title_version = static_cast<u16>(title_node.child("Version").text().as_uint());
    info.titles.push_back({title_id, title_version});
  }
  return info;
}

bool OnlineSystemUpdater::ShouldInstallTitle(const TitleInfo& title)
{
  const auto& es = m_ios.GetESCore();
  const auto installed_tmd = es.FindInstalledTMD(title.id);
  return !(installed_tmd.IsValid() && installed_tmd.GetTitleVersion() >= title.version &&
           es.GetStoredContentsFromTMD(installed_tmd).size() == installed_tmd.GetNumContents());
}

constexpr const char* GET_SYSTEM_TITLES_REQUEST_PAYLOAD = R"(<?xml version="1.0" encoding="UTF-8"?>
<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"
  xmlns:xsd="http://www.w3.org/2001/XMLSchema"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <soapenv:Body>
    <GetSystemUpdateRequest xmlns="urn:nus.wsapi.broadon.com">
      <Version>1.0</Version>
      <MessageId>0</MessageId>
      <DeviceId></DeviceId>
      <RegionId></RegionId>
    </GetSystemUpdateRequest>
  </soapenv:Body>
</soapenv:Envelope>
)";

OnlineSystemUpdater::Response OnlineSystemUpdater::GetSystemTitles()
{
  // Construct the request by loading the template first, then updating some fields.
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_string(GET_SYSTEM_TITLES_REQUEST_PAYLOAD);
  ASSERT(result);

  // Nintendo does not really care about the device ID or verify that we *are* that device,
  // as long as it is a valid Wii device ID.
  const std::string device_id = GetDeviceId();
  ASSERT(doc.select_node("//DeviceId").node().text().set(device_id.c_str()));

  // Write the correct device region.
  const std::string region = m_requested_region.empty() ? GetDeviceRegion() : m_requested_region;
  ASSERT(doc.select_node("//RegionId").node().text().set(region.c_str()));

  std::ostringstream stream;
  doc.save(stream);
  const std::string request = stream.str();

  std::string base_url = Config::Get(Config::MAIN_WII_NUS_SHOP_URL);
  if (base_url.empty())
  {
    // The NUS servers for the Wii are offline (https://bugs.dolphin-emu.org/issues/12865),
    // but the backing data CDN is still active and accessible from other URLs. We take advantage
    // of this by hosting our own NetUpdateSOAP endpoint which serves the correct list of titles to
    // install along with URLs for the Wii U CDN.
    base_url = "https://fakenus.dolphin-emu.org";
  }

  const std::string url = fmt::format("{}/nus/services/NetUpdateSOAP", base_url);
  const Common::HttpRequest::Response response =
      m_http.Post(url, request,
                  {
                      {"SOAPAction", "urn:nus.wsapi.broadon.com/GetSystemUpdate"},
                      {"User-Agent", "wii libnup/1.0"},
                      {"Content-Type", "text/xml; charset=utf-8"},
                  });

  if (!response)
    return {};
  return ParseTitlesResponse(*response);
}

UpdateResult OnlineSystemUpdater::DoOnlineUpdate()
{
  const Response info = GetSystemTitles();
  if (info.titles.empty())
    return UpdateResult::ServerFailed;

  // Download and install any title that is older than the NUS version.
  // The order is determined by the server response, which is: boot2, System Menu, IOSes, channels.
  // As we install any IOS required by titles, the real order is boot2, SM IOS, SM, IOSes, channels.
  std::unordered_set<u64> updated_titles;
  size_t processed = 0;
  for (const TitleInfo& title : info.titles)
  {
    if (!m_update_callback(processed++, info.titles.size(), title.id))
      return UpdateResult::Cancelled;

    const UpdateResult res = InstallTitleFromNUS(info.content_prefix_url, title, &updated_titles);
    if (res != UpdateResult::Succeeded)
    {
      ERROR_LOG_FMT(CORE, "Failed to update {:016x} -- aborting update", title.id);
      return res;
    }

    m_update_callback(processed, info.titles.size(), title.id);
  }

  if (updated_titles.empty())
  {
    NOTICE_LOG_FMT(CORE, "Update finished - Already up-to-date");
    return UpdateResult::AlreadyUpToDate;
  }
  NOTICE_LOG_FMT(CORE, "Update finished - {} updates installed", updated_titles.size());
  return UpdateResult::Succeeded;
}

UpdateResult OnlineSystemUpdater::InstallTitleFromNUS(const std::string& prefix_url,
                                                      const TitleInfo& title,
                                                      std::unordered_set<u64>* updated_titles)
{
  // We currently don't support boot2 updates at all, so ignore any attempt to install it.
  if (title.id == Titles::BOOT2)
    return UpdateResult::Succeeded;

  if (!ShouldInstallTitle(title) || updated_titles->contains(title.id))
    return UpdateResult::Succeeded;

  NOTICE_LOG_FMT(CORE, "Updating title {:016x}", title.id);

  // Download the ticket and certificates.
  const auto ticket = DownloadTicket(prefix_url, title);
  if (ticket.first.empty() || ticket.second.empty())
  {
    ERROR_LOG_FMT(CORE, "Failed to download ticket and certs");
    return UpdateResult::DownloadFailed;
  }

  // Import the ticket.
  IOS::HLE::ReturnCode ret = IOS::HLE::IPC_SUCCESS;
  auto& es = m_ios.GetESCore();
  if ((ret = es.ImportTicket(ticket.first, ticket.second)) < 0)
  {
    ERROR_LOG_FMT(CORE, "Failed to import ticket: error {}", std::to_underlying(ret));
    return UpdateResult::ImportFailed;
  }

  // Download the TMD.
  const auto tmd = DownloadTMD(prefix_url, title);
  if (!tmd.first.IsValid())
  {
    ERROR_LOG_FMT(CORE, "Failed to download TMD");
    return UpdateResult::DownloadFailed;
  }

  // Download and import any required system title first.
  const u64 ios_id = tmd.first.GetIOSId();
  if (ios_id != 0 && IOS::ES::IsTitleType(ios_id, IOS::ES::TitleType::System))
  {
    if (!es.FindInstalledTMD(ios_id).IsValid())
    {
      WARN_LOG_FMT(CORE, "Importing required system title {:016x} first", ios_id);
      const UpdateResult res = InstallTitleFromNUS(prefix_url, {ios_id, 0}, updated_titles);
      if (res != UpdateResult::Succeeded)
      {
        ERROR_LOG_FMT(CORE, "Failed to import required system title {:016x}", ios_id);
        return res;
      }
    }
  }

  // Initialise the title import.
  IOS::HLE::ESCore::Context context;
  if ((ret = es.ImportTitleInit(context, tmd.first.GetBytes(), tmd.second)) < 0)
  {
    ERROR_LOG_FMT(CORE, "Failed to initialise title import: error {}", std::to_underlying(ret));
    return UpdateResult::ImportFailed;
  }

  // Now download and install contents listed in the TMD.
  const std::vector<IOS::ES::Content> stored_contents = es.GetStoredContentsFromTMD(tmd.first);
  const UpdateResult import_result = [&] {
    for (const IOS::ES::Content& content : tmd.first.GetContents())
    {
      const bool is_already_installed =
          Common::Contains(stored_contents, content.id, &IOS::ES::Content::id);

      // Do skip what is already installed on the NAND.
      if (is_already_installed)
        continue;

      if ((ret = es.ImportContentBegin(context, title.id, content.id)) < 0)
      {
        ERROR_LOG_FMT(CORE, "Failed to initialise import for content {:08x}: error {}", content.id,
                      std::to_underlying(ret));
        return UpdateResult::ImportFailed;
      }

      const std::optional<std::vector<u8>> data = DownloadContent(prefix_url, title, content.id);
      if (!data)
      {
        ERROR_LOG_FMT(CORE, "Failed to download content {:08x}", content.id);
        return UpdateResult::DownloadFailed;
      }

      if (es.ImportContentData(context, 0, data->data(), static_cast<u32>(data->size())) < 0 ||
          es.ImportContentEnd(context, 0) < 0)
      {
        ERROR_LOG_FMT(CORE, "Failed to import content {:08x}", content.id);
        return UpdateResult::ImportFailed;
      }
    }
    return UpdateResult::Succeeded;
  }();
  const bool all_contents_imported = import_result == UpdateResult::Succeeded;

  if ((all_contents_imported && (ret = es.ImportTitleDone(context)) < 0) ||
      (!all_contents_imported && (ret = es.ImportTitleCancel(context)) < 0))
  {
    ERROR_LOG_FMT(CORE, "Failed to finalise title import: error {}", std::to_underlying(ret));
    return UpdateResult::ImportFailed;
  }

  if (!all_contents_imported)
    return import_result;

  updated_titles->emplace(title.id);
  return UpdateResult::Succeeded;
}

std::pair<IOS::ES::TMDReader, std::vector<u8>>
OnlineSystemUpdater::DownloadTMD(const std::string& prefix_url, const TitleInfo& title)
{
  const std::string url = (title.version == 0) ?
                              fmt::format("{}/{:016x}/tmd", prefix_url, title.id) :
                              fmt::format("{}/{:016x}/tmd.{}", prefix_url, title.id, title.version);
  const Common::HttpRequest::Response response = m_http.Get(url);
  if (!response)
    return {};

  // Too small to contain both the TMD and a cert chain.
  if (response->size() <= sizeof(IOS::ES::TMDHeader))
    return {};
  const size_t tmd_size =
      sizeof(IOS::ES::TMDHeader) +
      sizeof(IOS::ES::Content) *
          Common::swap16(response->data() + offsetof(IOS::ES::TMDHeader, num_contents));
  if (response->size() <= tmd_size)
    return {};

  const auto tmd_begin = response->begin();
  const auto tmd_end = tmd_begin + tmd_size;

  return {IOS::ES::TMDReader(std::vector<u8>(tmd_begin, tmd_end)),
          std::vector<u8>(tmd_end, response->end())};
}

std::pair<std::vector<u8>, std::vector<u8>>
OnlineSystemUpdater::DownloadTicket(const std::string& prefix_url, const TitleInfo& title)
{
  const std::string url = fmt::format("{}/{:016x}/cetk", prefix_url, title.id);
  const Common::HttpRequest::Response response = m_http.Get(url);
  if (!response)
    return {};

  // Too small to contain both the ticket and a cert chain.
  if (response->size() <= sizeof(IOS::ES::Ticket))
    return {};

  const auto ticket_begin = response->begin();
  const auto ticket_end = ticket_begin + sizeof(IOS::ES::Ticket);
  return {std::vector<u8>(ticket_begin, ticket_end), std::vector<u8>(ticket_end, response->end())};
}

std::optional<std::vector<u8>> OnlineSystemUpdater::DownloadContent(const std::string& prefix_url,
                                                                    const TitleInfo& title, u32 cid)
{
  const std::string url = fmt::format("{}/{:016x}/{:08x}", prefix_url, title.id, cid);
  return m_http.Get(url);
}

class DiscSystemUpdater final : public SystemUpdater
{
public:
  DiscSystemUpdater(UpdateCallback update_callback, const std::string& image_path)
      : m_update_callback{std::move(update_callback)}, m_volume{DiscIO::CreateDisc(image_path)}
  {
  }
  UpdateResult DoDiscUpdate();

private:
#pragma pack(push, 1)
  struct ManifestHeader
  {
    char timestamp[0x10];  // YYYY/MM/DD
    // There is a u32 in newer info files to indicate the number of entries,
    // but it's not used in older files, and it's not always at the same offset.
    // Too unreliable to use it.
    u32 padding[4];
  };
  static_assert(sizeof(ManifestHeader) == 32, "Wrong size");

  struct Entry
  {
    u32 type;
    u32 attribute;
    u32 unknown1;
    u32 unknown2;
    char path[0x40];
    u64 title_id;
    u16 title_version;
    u16 unused1[3];
    char name[0x40];
    char info[0x40];
    u8 unused2[0x120];
  };
  static_assert(sizeof(Entry) == 512, "Wrong size");
#pragma pack(pop)

  UpdateResult UpdateFromManifest(std::string_view manifest_name);
  UpdateResult ProcessEntry(u32 type, std::bitset<32> attrs, const TitleInfo& title,
                            std::string_view path);

  UpdateCallback m_update_callback;
  std::unique_ptr<DiscIO::VolumeDisc> m_volume;
  DiscIO::Partition m_partition;
};

UpdateResult DiscSystemUpdater::DoDiscUpdate()
{
  if (!m_volume)
    return UpdateResult::DiscReadFailed;

  // Do not allow mismatched regions, because installing an update will automatically change
  // the Wii's region and may result in semi/full system menu bricks.
  const IOS::ES::TMDReader system_menu_tmd =
      m_ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
  if (system_menu_tmd.IsValid() && m_volume->GetRegion() != system_menu_tmd.GetRegion())
    return UpdateResult::RegionMismatch;

  const auto partitions = m_volume->GetPartitions();
  const auto update_partition = std::ranges::find(
      partitions, DiscIO::PARTITION_UPDATE,
      [&](const DiscIO::Partition& partition) { return m_volume->GetPartitionType(partition); });

  if (update_partition == partitions.cend())
  {
    ERROR_LOG_FMT(CORE, "Could not find any update partition");
    return UpdateResult::MissingUpdatePartition;
  }

  m_partition = *update_partition;

  return UpdateFromManifest("__update.inf");
}

UpdateResult DiscSystemUpdater::UpdateFromManifest(std::string_view manifest_name)
{
  const DiscIO::FileSystem* disc_fs = m_volume->GetFileSystem(m_partition);
  if (!disc_fs)
  {
    ERROR_LOG_FMT(CORE, "Could not read the update partition file system");
    return UpdateResult::DiscReadFailed;
  }

  const std::unique_ptr<DiscIO::FileInfo> update_manifest = disc_fs->FindFileInfo(manifest_name);
  if (!update_manifest ||
      (update_manifest->GetSize() - sizeof(ManifestHeader)) % sizeof(Entry) != 0)
  {
    ERROR_LOG_FMT(CORE, "Invalid or missing update manifest");
    return UpdateResult::DiscReadFailed;
  }

  const u32 num_entries = (update_manifest->GetSize() - sizeof(ManifestHeader)) / sizeof(Entry);
  if (num_entries > 200)
    return UpdateResult::DiscReadFailed;

  std::vector<u8> entry(sizeof(Entry));
  size_t updates_installed = 0;
  for (u32 i = 0; i < num_entries; ++i)
  {
    const u32 offset = sizeof(ManifestHeader) + sizeof(Entry) * i;
    if (entry.size() != DiscIO::ReadFile(*m_volume, m_partition, update_manifest.get(),
                                         entry.data(), entry.size(), offset))
    {
      ERROR_LOG_FMT(CORE, "Failed to read update information from update manifest");
      return UpdateResult::DiscReadFailed;
    }

    const u32 type = Common::swap32(entry.data() + offsetof(Entry, type));
    const std::bitset<32> attrs = Common::swap32(entry.data() + offsetof(Entry, attribute));
    const u64 title_id = Common::swap64(entry.data() + offsetof(Entry, title_id));
    const u16 title_version = Common::swap16(entry.data() + offsetof(Entry, title_version));
    const char* path_pointer = reinterpret_cast<const char*>(entry.data() + offsetof(Entry, path));
    const std::string_view path{path_pointer, strnlen(path_pointer, sizeof(Entry::path))};

    if (!m_update_callback(i, num_entries, title_id))
      return UpdateResult::Cancelled;

    const UpdateResult res = ProcessEntry(type, attrs, {title_id, title_version}, path);
    if (res != UpdateResult::Succeeded && res != UpdateResult::AlreadyUpToDate)
    {
      ERROR_LOG_FMT(CORE, "Failed to update {:016x} -- aborting update", title_id);
      return res;
    }

    if (res == UpdateResult::Succeeded)
      ++updates_installed;
  }
  return updates_installed == 0 ? UpdateResult::AlreadyUpToDate : UpdateResult::Succeeded;
}

UpdateResult DiscSystemUpdater::ProcessEntry(u32 type, std::bitset<32> attrs,
                                             const TitleInfo& title, std::string_view path)
{
  // Skip any unknown type and boot2 updates (for now).
  if (type != 2 && type != 3 && type != 6 && type != 7)
    return UpdateResult::AlreadyUpToDate;

  const IOS::ES::TMDReader tmd = m_ios.GetESCore().FindInstalledTMD(title.id);
  const IOS::ES::TicketReader ticket = m_ios.GetESCore().FindSignedTicket(title.id);

  // Optional titles can be skipped if the ticket is present, even when the title isn't installed.
  if (attrs.test(16) && ticket.IsValid())
    return UpdateResult::AlreadyUpToDate;

  // Otherwise, the title is only skipped if it is installed, its ticket is imported,
  // and the installed version is new enough. No further checks unlike the online updater.
  if (tmd.IsValid() && tmd.GetTitleVersion() >= title.version)
    return UpdateResult::AlreadyUpToDate;

  // Import the WAD.
  auto blob = DiscIO::VolumeFileBlobReader::Create(*m_volume, m_partition, path);
  if (!blob)
  {
    ERROR_LOG_FMT(CORE, "Could not find {}", path);
    return UpdateResult::DiscReadFailed;
  }
  const DiscIO::VolumeWAD wad{std::move(blob)};
  const bool success = ImportWAD(m_ios, wad, IOS::HLE::ESCore::VerifySignature::Yes);
  return success ? UpdateResult::Succeeded : UpdateResult::ImportFailed;
}

UpdateResult DoOnlineUpdate(UpdateCallback update_callback, const std::string& region)
{
  OnlineSystemUpdater updater{std::move(update_callback), region};
  return updater.DoOnlineUpdate();
}

UpdateResult DoDiscUpdate(UpdateCallback update_callback, const std::string& image_path)
{
  DiscSystemUpdater updater{std::move(update_callback), image_path};
  return updater.DoDiscUpdate();
}

static NANDCheckResult CheckNAND(IOS::HLE::Kernel& ios, bool repair)
{
  NANDCheckResult result;
  const auto& es = ios.GetESCore();

  // Check for NANDs that were used with old Dolphin versions.
  const std::string sys_replace_path =
      Common::RootUserPath(Common::FromWhichRoot::Configured) + "/sys/replace";
  if (File::Exists(sys_replace_path))
  {
    ERROR_LOG_FMT(CORE,
                  "CheckNAND: NAND was used with old versions, so it is likely to be damaged");
    if (repair)
      File::Delete(sys_replace_path);
    else
      result.bad = true;
  }

  // Clean up after a bug fixed in https://github.com/dolphin-emu/dolphin/pull/8802
  const std::string rfl_db_path = Common::GetMiiDatabasePath(Common::FromWhichRoot::Configured);
  const File::FileInfo rfl_db(rfl_db_path);
  if (rfl_db.Exists() && rfl_db.GetSize() == 0)
  {
    ERROR_LOG_FMT(CORE, "CheckNAND: RFL_DB.dat exists but is empty");
    if (repair)
      File::Delete(rfl_db_path);
    else
      result.bad = true;
  }

  for (const u64 title_id : es.GetInstalledTitles())
  {
    const std::string title_dir = Common::GetTitlePath(title_id, Common::FromWhichRoot::Configured);
    const std::string content_dir = title_dir + "/content";
    const std::string data_dir = title_dir + "/data";

    // Check for missing title sub directories.
    for (const std::string& dir : {content_dir, data_dir})
    {
      if (File::IsDirectory(dir))
        continue;

      ERROR_LOG_FMT(CORE, "CheckNAND: Missing dir {} for title {:016x}", dir, title_id);
      if (repair)
        File::CreateDir(dir);
      else
        result.bad = true;
    }

    // Check for incomplete title installs (missing ticket, TMD or contents).
    const auto ticket = es.FindSignedTicket(title_id);
    if (!IOS::ES::IsDiscTitle(title_id) && !ticket.IsValid())
    {
      ERROR_LOG_FMT(CORE, "CheckNAND: Missing ticket for title {:016x}", title_id);
      result.titles_to_remove.insert(title_id);
      if (repair)
        File::DeleteDirRecursively(title_dir);
      else
        result.bad = true;
    }

    const auto tmd = es.FindInstalledTMD(title_id);
    if (!tmd.IsValid())
    {
      if (File::ScanDirectoryTree(content_dir, false).children.empty())
      {
        WARN_LOG_FMT(CORE, "CheckNAND: Missing TMD for title {:016x}", title_id);
      }
      else
      {
        ERROR_LOG_FMT(CORE, "CheckNAND: Missing TMD for title {:016x}", title_id);
        result.titles_to_remove.insert(title_id);
        if (repair)
          File::DeleteDirRecursively(title_dir);
        else
          result.bad = true;
      }
      // Further checks require the TMD to be valid.
      continue;
    }

    const auto installed_contents = es.GetStoredContentsFromTMD(tmd);
    const bool is_installed = std::ranges::any_of(
        installed_contents, [](const auto& content) { return !content.IsShared(); });

    if (is_installed && installed_contents != tmd.GetContents() &&
        (tmd.GetTitleFlags() & IOS::ES::TitleFlags::TITLE_TYPE_DATA) == 0)
    {
      ERROR_LOG_FMT(CORE, "CheckNAND: Missing contents for title {:016x}", title_id);
      result.titles_to_remove.insert(title_id);
      if (repair)
        File::DeleteDirRecursively(title_dir);
      else
        result.bad = true;
    }
  }

  // Get some storage stats.
  const auto fs = ios.GetFS();
  const auto root_stats = fs->GetExtendedDirectoryStats("/");

  // The Wii System Menu's save/channel management only considers a specific subset of the Wii NAND
  // user-accessible and will only use those folders when calculating the amount of free blocks it
  // displays. This can have weird side-effects where the other parts of the NAND contain more data
  // than reserved and it will display free blocks even though there isn't any space left. To avoid
  // confusion, report the 'user' and 'system' data separately to the user.
  u64 used_clusters_user = 0;
  u64 used_inodes_user = 0;
  for (std::string user_path : {"/meta", "/ticket", "/title/00010000", "/title/00010001",
                                "/title/00010003", "/title/00010004", "/title/00010005",
                                "/title/00010006", "/title/00010007", "/shared2/title"})
  {
    const auto dir_stats = fs->GetExtendedDirectoryStats(user_path);
    if (dir_stats)
    {
      used_clusters_user += dir_stats->used_clusters;
      used_inodes_user += dir_stats->used_inodes;
    }
  }

  result.used_clusters_user = used_clusters_user;
  result.used_clusters_system = root_stats ? (root_stats->used_clusters - used_clusters_user) : 0;
  result.used_inodes_user = used_inodes_user;
  result.used_inodes_system = root_stats ? (root_stats->used_inodes - used_inodes_user) : 0;

  return result;
}

NANDCheckResult CheckNAND(IOS::HLE::Kernel& ios)
{
  return CheckNAND(ios, false);
}

bool RepairNAND(IOS::HLE::Kernel& ios)
{
  return !CheckNAND(ios, true).bad;
}

static std::shared_ptr<IOS::HLE::Device> GetBluetoothDevice()
{
  auto* ios = Core::System::GetInstance().GetIOS();
  return ios ? ios->GetDeviceByName("/dev/usb/oh1/57e/305") : nullptr;
}

std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice()
{
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothEmuDevice>(GetBluetoothDevice());
}

std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice()
{
  if (!Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    return nullptr;
  return std::static_pointer_cast<IOS::HLE::BluetoothRealDevice>(GetBluetoothDevice());
}
}  // namespace WiiUtils
