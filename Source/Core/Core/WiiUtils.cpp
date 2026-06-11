// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
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

// A full forwarders.json row (the lazy lookup map only retains title_id -> disc_path).
struct ForwarderMapEntry
{
  u64 title_id = 0;
  std::string game_id;
  u16 revision = 0;
  std::string disc_path;
  std::string long_name;
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
    ForwarderMapEntry e;
    e.title_id = std::stoull(*tid, nullptr, 16);
    e.disc_path = *disc;
    e.game_id = ReadStringFromJson(row, "game_id").value_or("");
    e.long_name = ReadStringFromJson(row, "long_name").value_or("");
    const auto rev = row.find("revision");
    if (rev != row.end() && rev->second.is<double>())
      e.revision = static_cast<u16>(rev->second.get<double>());
    entries.push_back(std::move(e));
  }
  return entries;
}

// Serialize the forwarder map (same schema InstallForwardersForLibrary writes).
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
    row["long_name"] = picojson::value(e.long_name);
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

// A known-good "donor" channel banner (captured from a menu-safe game). We reuse
// its asset payload (icon/banner/sound) verbatim and only patch in per-game title
// text, so a quirky disc banner can never crash the System Menu's grid renderer.
// Loaded once from the user dir; null if missing/invalid.
const std::vector<u8>* GetDonorBanner()
{
  static const std::optional<std::vector<u8>> s_donor = []() -> std::optional<std::vector<u8>> {
    const std::string path = File::GetUserPath(D_USER_IDX) + "forwarder_donor.bnr";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
      ERROR_LOG_FMT(IOS_ES, "Forwarder: donor banner not found at {}", path);
      return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    std::vector<u8> data(static_cast<size_t>(size > 0 ? size : 0));
    if (size < static_cast<std::streamoff>(IMET_HEADER_SIZE + 4) ||
        !file.read(reinterpret_cast<char*>(data.data()), size) || data[0x40] != 'I' ||
        data[0x41] != 'M' || data[0x42] != 'E' || data[0x43] != 'T')
    {
      ERROR_LOG_FMT(IOS_ES, "Forwarder: donor banner at {} is missing/invalid", path);
      return std::nullopt;
    }
    return data;
  }();
  return s_donor ? &*s_donor : nullptr;
}

// Games whose disc opening.bnr crashes the System Menu's channel-grid renderer when
// reused as a channel banner; we fall back to the safe donor tile for these. Seeded
// with known-bad ids, and extended via the optional file <user>/forwarder_blocklist.txt
// (one game id per line, '#' for comments) so users can flag a bad game without a rebuild.
bool IsBannerBlocklisted(const std::string& game_id)
{
  static const std::set<std::string> s_blocklist = [] {
    std::set<std::string> set{"SSQE01"};  // Mario Party 9 (USA, Asia)
    std::ifstream file(File::GetUserPath(D_USER_IDX) + "forwarder_blocklist.txt");
    for (std::string line; std::getline(file, line);)
    {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      if (!line.empty() && line.front() != '#')
        set.insert(line);
    }
    return set;
  }();
  return s_blocklist.count(game_id) != 0;
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
    // Mark loaded even on failure so we don't re-hit the disk on every launch.
    s_forwarder_map_loaded = true;
    const std::string path = File::GetUserPath(D_USER_IDX) + "forwarders.json";
    picojson::value root;
    std::string error;
    if (JsonFromFile(path, &root, &error) && root.is<picojson::object>())
    {
      const picojson::object& obj = root.get<picojson::object>();
      const auto forwarders = obj.find("forwarders");
      if (forwarders != obj.end() && forwarders->second.is<picojson::array>())
      {
        for (const picojson::value& entry : forwarders->second.get<picojson::array>())
        {
          if (!entry.is<picojson::object>())
            continue;
          const picojson::object& row = entry.get<picojson::object>();
          const std::optional<std::string> tid = ReadStringFromJson(row, "title_id");
          const std::optional<std::string> disc = ReadStringFromJson(row, "disc_path");
          if (tid && disc)
            s_forwarder_map.emplace(std::stoull(*tid, nullptr, 16), *disc);
        }
      }
    }
    else if (!error.empty())
    {
      WARN_LOG_FMT(IOS_ES, "Forwarder map: failed to parse {}: {}", path, error);
    }
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

  // Prefer the game's own banner (real per-game icon + animation); fall back to the safe
  // donor tile for banners known to crash the menu, or ones we can't read.
  std::optional<std::vector<u8>> banner;
  if (IsBannerBlocklisted(game_id))
  {
    banner = BuildSafeBanner(*volume, partition);
  }
  else
  {
    banner = ReadFullBanner(*volume, partition);
    if (!banner)
      banner = BuildSafeBanner(*volume, partition);
  }
  if (!banner)
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder: '{}' ({}) - no usable banner; skipping", disc_path, game_id);
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

size_t InstallForwardersForLibrary(const std::vector<ForwarderLibraryEntry>& games)
{
  picojson::array rows;
  for (const ForwarderLibraryEntry& game : games)
  {
    const std::optional<ForwarderInfo> info = InstallForwarder(game.disc_path);
    if (!info)
      continue;
    picojson::object row;
    row["title_id"] = picojson::value(fmt::format("{:016x}", info->title_id));
    row["game_id"] = picojson::value(info->game_id);
    row["revision"] = picojson::value(static_cast<double>(info->revision));
    row["disc_path"] = picojson::value(game.disc_path);
    row["long_name"] = picojson::value(game.long_name);
    rows.emplace_back(std::move(row));
  }

  picojson::object doc;
  doc["version"] = picojson::value(1.0);
  doc["forwarders"] = picojson::value(rows);
  const std::string path = File::GetUserPath(D_USER_IDX) + "forwarders.json";
  if (!JsonToFile(path, picojson::value(doc), true))
    ERROR_LOG_FMT(IOS_ES, "Forwarder: failed to write map {}", path);

  ReloadForwarderMap();
  INFO_LOG_FMT(IOS_ES, "Forwarder sync: installed {} of {} games", rows.size(), games.size());
  return rows.size();
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

ForwarderSyncResult SyncForwardersWithLibrary(const std::vector<std::string>& current_disc_paths)
{
  ForwarderSyncResult result;
  if (s_forwarder_sync_running.exchange(true))
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder sync already in progress; skipping");
    return result;
  }
  const Common::ScopeGuard running_guard{[] { s_forwarder_sync_running.store(false); }};

  // Phase 1: the existing map is the authoritative "installed" set.
  const std::vector<ForwarderMapEntry> old_entries = LoadForwarderMapFull();
  std::map<std::string, ForwarderMapEntry> old_by_path;
  std::map<u64, ForwarderMapEntry> old_by_tid;
  for (const ForwarderMapEntry& e : old_entries)
  {
    old_by_path.emplace(e.disc_path, e);
    old_by_tid.emplace(e.title_id, e);
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
      desired.push_back({tid, game_id, revision, path, std::string{}});
      desired_tids.insert(tid);
      ++result.already_present;
      continue;
    }
    const std::optional<ForwarderInfo> info = InstallForwarder(path);
    if (!info)
      continue;
    desired.push_back({info->title_id, info->game_id, info->revision, path, std::string{}});
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

  INFO_LOG_FMT(IOS_ES, "Forwarder sync: installed={} moved={} present={} uninstalled={} (map {})",
               result.installed, result.moved, result.already_present, result.uninstalled,
               result.map_modified ? "updated" : "unchanged");
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
