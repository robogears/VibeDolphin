// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiBanner.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mbedtls/md5.h>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace WiiUtils::Banner
{
namespace
{
// ---------------------------------------------------------------------------
// Big-endian readers/writers with explicit bounds checks. Every parse below is
// defensive: a malformed banner must make us bail to the safe fallback, never
// read out of bounds (an OOB read here would be our own crash).
// ---------------------------------------------------------------------------
bool InBounds(const std::vector<u8>& d, size_t off, size_t len)
{
  return off <= d.size() && len <= d.size() - off;
}
u16 RU16(const std::vector<u8>& d, size_t o)
{
  return static_cast<u16>((d[o] << 8) | d[o + 1]);
}
u32 RU32(const std::vector<u8>& d, size_t o)
{
  return (u32{d[o]} << 24) | (u32{d[o + 1]} << 16) | (u32{d[o + 2]} << 8) | u32{d[o + 3]};
}
void WU32(std::vector<u8>& d, size_t o, u32 v)
{
  d[o] = static_cast<u8>(v >> 24);
  d[o + 1] = static_cast<u8>(v >> 16);
  d[o + 2] = static_cast<u8>(v >> 8);
  d[o + 3] = static_cast<u8>(v);
}

constexpr u32 U8_MAGIC = 0x55AA382D;
constexpr u32 TPL_MAGIC = 0x0020AF30;
constexpr u32 LZ77_MAGIC = 0x4C5A3737;  // "LZ77"
constexpr size_t IMD5_HEADER_SIZE = 0x20;

// IMET banner layout (offsets within an opening.bnr / channel banner; see WiiUtils.cpp).
constexpr size_t IMET_SIZE = 0x600;       // header size; the outer U8 archive follows it
constexpr size_t IMET_TITLES_OFF = 0x5C;  // 10-language title block
constexpr size_t IMET_TITLES_LEN = 0x348;
constexpr size_t IMET_MD5_OFF = 0x5F0;  // MD5 over [0, IMET_SIZE) with this field zeroed
// IMET fields recording each meta file's byte size (must match the outer U8 entries).
constexpr std::array<std::pair<const char*, size_t>, 3> IMET_META_SIZE_FIELDS = {
    {{"icon.bin", 0x4C}, {"banner.bin", 0x50}, {"sound.bin", 0x54}}};

// TPL texture format codes (libtpl / GC-Wii).
enum : u32
{
  FMT_I4 = 0,
  FMT_I8 = 1,
  FMT_IA4 = 2,
  FMT_IA8 = 3,
  FMT_RGB565 = 4,
  FMT_RGB5A3 = 5,
  FMT_RGBA8 = 6,
  FMT_CI4 = 8,
  FMT_CI8 = 9,
  FMT_CI14X2 = 10,
  FMT_CMPR = 14,
};

// Encoded byte size of a texture of the given format/dimensions (dimensions are padded up
// to the format's block size first). Returns 0 for formats we don't model.
size_t TextureDataSize(u32 format, u32 w, u32 h)
{
  const auto pad = [](u32 v, u32 a) { return Common::AlignUp(v, a); };
  switch (format)
  {
  case FMT_RGB565:
  case FMT_RGB5A3:
  case FMT_IA8:
  case FMT_CI14X2:
    return size_t{pad(w, 4)} * pad(h, 4) * 2;
  case FMT_RGBA8:
    return size_t{pad(w, 4)} * pad(h, 4) * 4;
  case FMT_I8:
  case FMT_IA4:
  case FMT_CI8:
    return size_t{pad(w, 8)} * pad(h, 4);
  case FMT_I4:
  case FMT_CI4:
  case FMT_CMPR:
    return size_t{pad(w, 8)} * pad(h, 8) / 2;
  default:
    return 0;
  }
}

// 5/6-bit -> 8-bit expansion by bit replication (matches Dolphin's LookUpTables).
constexpr u8 Conv3To8(u8 v)
{
  return static_cast<u8>((v << 5) | (v << 2) | (v >> 1));
}
constexpr u8 Conv4To8(u8 v)
{
  return static_cast<u8>((v << 4) | v);
}
constexpr u8 Conv5To8(u8 v)
{
  return static_cast<u8>((v << 3) | (v >> 2));
}
constexpr u8 Conv6To8(u8 v)
{
  return static_cast<u8>((v << 2) | (v >> 4));
}

// RGB5A3 stores opaque pixels as 555 (top bit set) and translucent ones as 4443. On encode,
// alpha at/above this threshold is treated as opaque (matches the libtpl reference: a 3-bit
// alpha of 7 -> 255, i.e. anything rounding to fully opaque uses the 555 path).
constexpr u8 RGB5A3_OPAQUE_THRESHOLD = 0xDA;
// Round-to-nearest quantization of an 8-bit channel to `bits` (e.g. 5/4/3-bit fields).
constexpr u8 QuantizeTo(u32 v8, u32 max)
{
  return static_cast<u8>((v8 * max + 127) / 255);
}

bool CanEncode(u32 format)
{
  return format == FMT_RGB5A3 || format == FMT_RGBA8;
}
bool CanDecode(u32 format)
{
  return format == FMT_RGB5A3 || format == FMT_RGBA8 || format == FMT_RGB565;
}

// ---------------------------------------------------------------------------
// Pixel buffers are u32 0xAARRGGBB, with true per-pixel alpha preserved end-to-end.
// ---------------------------------------------------------------------------
struct Image
{
  u32 w = 0, h = 0;
  std::vector<u32> px;  // w*h, row-major
};

// Decode a TPL texture's pixel region into an Image, cropping the block-padded
// decode down to the real width/height. nullopt for formats/inputs we can't handle.
std::optional<Image> DecodeTexture(u32 format, u32 w, u32 h, const u8* data, size_t data_size)
{
  if (w == 0 || h == 0 || w > 1024 || h > 1024)
    return std::nullopt;
  if (TextureDataSize(format, w, h) != data_size || data_size == 0)
    return std::nullopt;

  const u32 aw = Common::AlignUp(w, 4);
  const u32 ah = Common::AlignUp(h, 4);
  std::vector<u32> padded(size_t{aw} * ah);

  if (format == FMT_RGB5A3)
  {
    // Decode locally (NOT Common::Decode5A3Image, which discards alpha by pre-blending
    // translucent pixels against black) so the game's transparency survives the round-trip.
    size_t s = 0;
    for (u32 y = 0; y < ah; y += 4)
      for (u32 x = 0; x < aw; x += 4)
        for (u32 iy = 0; iy < 4; ++iy)
          for (u32 ix = 0; ix < 4; ++ix, s += 2)
          {
            const u16 v = static_cast<u16>((data[s] << 8) | data[s + 1]);
            u32 r, g, b, a;
            if (v & 0x8000)  // 555 opaque
            {
              a = 0xFF;
              r = Conv5To8((v >> 10) & 0x1F);
              g = Conv5To8((v >> 5) & 0x1F);
              b = Conv5To8(v & 0x1F);
            }
            else  // 4443 translucent
            {
              a = Conv3To8((v >> 12) & 0x7);
              r = Conv4To8((v >> 8) & 0xF);
              g = Conv4To8((v >> 4) & 0xF);
              b = Conv4To8(v & 0xF);
            }
            padded[(y + iy) * aw + (x + ix)] = (a << 24) | (r << 16) | (g << 8) | b;
          }
  }
  else if (format == FMT_RGB565)
  {
    size_t s = 0;
    for (u32 y = 0; y < ah; y += 4)
      for (u32 x = 0; x < aw; x += 4)
        for (u32 iy = 0; iy < 4; ++iy)
          for (u32 ix = 0; ix < 4; ++ix, s += 2)
          {
            const u16 v = static_cast<u16>((data[s] << 8) | data[s + 1]);
            const u32 r = Conv5To8((v >> 11) & 0x1F);
            const u32 g = Conv6To8((v >> 5) & 0x3F);
            const u32 b = Conv5To8(v & 0x1F);
            padded[(y + iy) * aw + (x + ix)] = 0xFF000000u | (r << 16) | (g << 8) | b;
          }
  }
  else if (format == FMT_RGBA8)
  {
    size_t t = 0;
    for (u32 y = 0; y < ah; y += 4)
      for (u32 x = 0; x < aw; x += 4, t += 64)
        for (u32 iy = 0; iy < 4; ++iy)
          for (u32 ix = 0; ix < 4; ++ix)
          {
            const size_t idx = (size_t{iy} * 4 + ix);
            const u8 a = data[t + 2 * idx];
            const u8 r = data[t + 2 * idx + 1];
            const u8 g = data[t + 32 + 2 * idx];
            const u8 bch = data[t + 32 + 2 * idx + 1];
            padded[(y + iy) * aw + (x + ix)] =
                (u32{a} << 24) | (u32{r} << 16) | (u32{g} << 8) | bch;
          }
  }
  else
  {
    return std::nullopt;
  }

  Image img;
  img.w = w;
  img.h = h;
  img.px.resize(size_t{w} * h);
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x)
      img.px[y * w + x] = padded[y * aw + x];
  return img;
}

// Bilinear resize of an 0xAARRGGBB image.
Image ResizeImage(const Image& s, u32 dw, u32 dh)
{
  Image d;
  d.w = dw;
  d.h = dh;
  d.px.resize(size_t{dw} * dh);
  if (s.w == 0 || s.h == 0)
    return d;
  for (u32 y = 0; y < dh; ++y)
  {
    const float fy = (dh > 1) ? (static_cast<float>(y) * (s.h - 1) / (dh - 1)) : 0.0f;
    const u32 y0 = static_cast<u32>(fy);
    const u32 y1 = std::min(y0 + 1, s.h - 1);
    const float wy = fy - y0;
    for (u32 x = 0; x < dw; ++x)
    {
      const float fx = (dw > 1) ? (static_cast<float>(x) * (s.w - 1) / (dw - 1)) : 0.0f;
      const u32 x0 = static_cast<u32>(fx);
      const u32 x1 = std::min(x0 + 1, s.w - 1);
      const float wx = fx - x0;
      const auto sample = [&](u32 px, u32 py) { return s.px[size_t{py} * s.w + px]; };
      float ch[4] = {0, 0, 0, 0};
      const u32 corners[4] = {sample(x0, y0), sample(x1, y0), sample(x0, y1), sample(x1, y1)};
      const float weights[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
      for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k)
          ch[c] += weights[k] * ((corners[k] >> (c * 8)) & 0xFF);
      u32 out = 0;
      for (int c = 0; c < 4; ++c)
        out |= (static_cast<u32>(ch[c] + 0.5f) & 0xFF) << (c * 8);
      d.px[size_t{y} * dw + x] = out;
    }
  }
  return d;
}

// Encode an 0xAARRGGBB image into a TPL texture format's raw pixel region. The image must
// already be the target width/height; output is block-padded. Returns empty for formats we
// don't encode. RGB5A3 packing mirrors libtpl (which inverts Common::Decode5A3Image).
std::vector<u8> EncodeTexture(u32 format, const Image& img)
{
  const u32 w = img.w, h = img.h;
  std::vector<u8> out(TextureDataSize(format, w, h), 0);
  if (out.empty())
    return out;
  const u32 aw = Common::AlignUp(w, 4);
  const u32 ah = Common::AlignUp(h, 4);
  const auto at = [&](u32 x, u32 y) -> u32 {
    return (x < w && y < h) ? img.px[size_t{y} * w + x] : 0u;
  };

  if (format == FMT_RGB5A3)
  {
    size_t z = 0;
    for (u32 y = 0; y < ah; y += 4)
      for (u32 x = 0; x < aw; x += 4)
        for (u32 iy = 0; iy < 4; ++iy)
          for (u32 ix = 0; ix < 4; ++ix, z += 2)
          {
            const u32 c = at(x + ix, y + iy);
            const u32 a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF,
                      b = c & 0xFF;
            u16 v;
            if (a <= RGB5A3_OPAQUE_THRESHOLD)  // 4443 (translucent)
              v = static_cast<u16>((QuantizeTo(a, 7) << 12) | (QuantizeTo(r, 15) << 8) |
                                   (QuantizeTo(g, 15) << 4) | QuantizeTo(b, 15));
            else  // 555 opaque
              v = static_cast<u16>(0x8000 | (QuantizeTo(r, 31) << 10) | (QuantizeTo(g, 31) << 5) |
                                   QuantizeTo(b, 31));
            out[z] = static_cast<u8>(v >> 8);
            out[z + 1] = static_cast<u8>(v);
          }
  }
  else if (format == FMT_RGBA8)
  {
    size_t t = 0;
    for (u32 y = 0; y < ah; y += 4)
      for (u32 x = 0; x < aw; x += 4, t += 64)
        for (u32 iy = 0; iy < 4; ++iy)
          for (u32 ix = 0; ix < 4; ++ix)
          {
            const u32 c = at(x + ix, y + iy);
            const size_t idx = (size_t{iy} * 4 + ix);
            out[t + 2 * idx] = static_cast<u8>(c >> 24);       // A
            out[t + 2 * idx + 1] = static_cast<u8>(c >> 16);   // R
            out[t + 32 + 2 * idx] = static_cast<u8>(c >> 8);   // G
            out[t + 32 + 2 * idx + 1] = static_cast<u8>(c);    // B
          }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Nintendo LZ10 (de)compression. We never need real compression: an all-literal
// stream (every flag bit 0) is a valid type-0x10 stream, so "recompress" just
// re-frames the bytes. We preserve whichever wrapper the donor used.
// ---------------------------------------------------------------------------
enum class LzVariant
{
  Lz77Magic,    // "LZ77" + (size|0x10<<24) LE + stream
  Lz10Bare,     // (size|0x10<<24) LE + stream
  Uncompressed  // payload is the U8 archive directly (no LZ)
};

// Decompress an IMD5 payload into the inner U8 bytes; reports which wrapper it used.
std::optional<std::vector<u8>> LzDecompress(const std::vector<u8>& p, LzVariant* variant)
{
  if (InBounds(p, 0, 4) && RU32(p, 0) == U8_MAGIC)
  {
    *variant = LzVariant::Uncompressed;
    return p;
  }
  size_t hdr;
  if (InBounds(p, 0, 8) && RU32(p, 0) == LZ77_MAGIC && (p[7] == 0x10))
  {
    *variant = LzVariant::Lz77Magic;
    hdr = 4;
  }
  else if (InBounds(p, 0, 4) && p[3] == 0x10)
  {
    *variant = LzVariant::Lz10Bare;
    hdr = 0;
  }
  else
  {
    return std::nullopt;
  }

  const size_t out_size = p[hdr] | (p[hdr + 1] << 8) | (p[hdr + 2] << 16);
  if (out_size == 0 || out_size > 4 * 1024 * 1024)
    return std::nullopt;
  std::vector<u8> out;
  out.reserve(out_size);
  size_t pos = hdr + 4;
  while (out.size() < out_size)
  {
    if (pos >= p.size())
      return std::nullopt;
    u8 flags = p[pos++];
    for (int i = 0; i < 8 && out.size() < out_size; ++i)
    {
      if (flags & 0x80)  // back-reference
      {
        if (pos + 1 >= p.size())
          return std::nullopt;
        const u8 b1 = p[pos++], b2 = p[pos++];
        const size_t len = (b1 >> 4) + 3;
        const size_t disp = (((b1 & 0xF) << 8) | b2) + 1;
        if (disp > out.size())
          return std::nullopt;
        const size_t start = out.size() - disp;
        for (size_t k = 0; k < len && out.size() < out_size; ++k)
          out.push_back(out[start + k]);
      }
      else  // literal
      {
        if (pos >= p.size())
          return std::nullopt;
        out.push_back(p[pos++]);
      }
      flags = static_cast<u8>(flags << 1);
    }
  }
  return out;
}

// Re-frame raw U8 bytes as an IMD5 payload using the given wrapper (all-literal LZ10).
std::vector<u8> LzCompressAllLiteral(const std::vector<u8>& raw, LzVariant variant)
{
  if (variant == LzVariant::Uncompressed)
    return raw;
  std::vector<u8> out;
  if (variant == LzVariant::Lz77Magic)
  {
    out.push_back('L');
    out.push_back('Z');
    out.push_back('7');
    out.push_back('7');
  }
  const size_t n = raw.size();
  out.push_back(static_cast<u8>(n));
  out.push_back(static_cast<u8>(n >> 8));
  out.push_back(static_cast<u8>(n >> 16));
  out.push_back(0x10);
  for (size_t i = 0; i < n;)
  {
    out.push_back(0x00);  // 8 literals follow
    for (int k = 0; k < 8 && i < n; ++k, ++i)
      out.push_back(raw[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// IMD5: "IMD5" + u32 size(BE) + 8 zero + 16-byte MD5 of the payload that follows.
// ---------------------------------------------------------------------------
std::optional<std::vector<u8>> Imd5Payload(const std::vector<u8>& f)
{
  if (!InBounds(f, 0, IMD5_HEADER_SIZE) || std::memcmp(f.data(), "IMD5", 4) != 0)
    return std::nullopt;
  const u32 size = RU32(f, 4);
  if (!InBounds(f, IMD5_HEADER_SIZE, size))
    return std::nullopt;
  return std::vector<u8>(f.begin() + IMD5_HEADER_SIZE, f.begin() + IMD5_HEADER_SIZE + size);
}

std::vector<u8> Imd5Wrap(const std::vector<u8>& payload)
{
  std::vector<u8> f(IMD5_HEADER_SIZE);
  std::memcpy(f.data(), "IMD5", 4);
  WU32(f, 4, static_cast<u32>(payload.size()));
  std::array<u8, 16> digest{};
  mbedtls_md5_ret(payload.data(), payload.size(), digest.data());
  std::copy(digest.begin(), digest.end(), f.begin() + 16);
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

// ---------------------------------------------------------------------------
// U8 archive parse + rebuild. We preserve the node tree exactly (directory nodes
// keep their parent/bound fields verbatim); only file data — and the offsets that
// follow from it — are recomputed.
// ---------------------------------------------------------------------------
struct U8Node
{
  bool is_dir = false;
  std::string name;
  u32 dir_parent = 0;  // dir only: parent node index (data_offset field)
  u32 dir_bound = 0;   // dir only: first node index NOT in this dir (size field)
  std::vector<u8> data;  // file only
};

std::optional<std::vector<U8Node>> U8Parse(const std::vector<u8>& a)
{
  if (!InBounds(a, 0, 0x20) || RU32(a, 0) != U8_MAGIC)
    return std::nullopt;
  const u32 root_off = RU32(a, 4);
  if (!InBounds(a, root_off, 12))
    return std::nullopt;
  const u32 count = RU32(a, root_off + 8);  // root node's size = total node count
  if (count == 0 || count > 4096 || !InBounds(a, root_off, size_t{count} * 12))
    return std::nullopt;
  const size_t strings = root_off + size_t{count} * 12;

  std::vector<U8Node> nodes;
  nodes.reserve(count);
  for (u32 i = 0; i < count; ++i)
  {
    const size_t no = root_off + size_t{i} * 12;
    U8Node n;
    n.is_dir = (a[no] == 1);
    const u32 name_off = (u32{a[no + 1]} << 16) | (u32{a[no + 2]} << 8) | a[no + 3];
    const u32 data_off = RU32(a, no + 4);
    const u32 size = RU32(a, no + 8);
    // name string (NUL-terminated) in the string table.
    size_t s = strings + name_off;
    while (s < a.size() && a[s] != 0)
      n.name.push_back(static_cast<char>(a[s++]));
    if (n.is_dir)
    {
      n.dir_parent = data_off;
      n.dir_bound = size;
    }
    else
    {
      if (!InBounds(a, data_off, size))
        return std::nullopt;
      n.data.assign(a.begin() + data_off, a.begin() + data_off + size);
    }
    nodes.push_back(std::move(n));
  }
  return nodes;
}

std::vector<u8> U8Build(const std::vector<U8Node>& nodes)
{
  const u32 count = static_cast<u32>(nodes.size());
  // String table: each name NUL-terminated, in node order (root name is empty).
  std::vector<u8> strtab;
  std::vector<u32> name_offsets(count);
  for (u32 i = 0; i < count; ++i)
  {
    name_offsets[i] = static_cast<u32>(strtab.size());
    strtab.insert(strtab.end(), nodes[i].name.begin(), nodes[i].name.end());
    strtab.push_back(0);
  }
  const u32 root_off = 0x20;
  const u32 header_size = count * 12 + static_cast<u32>(strtab.size());
  u32 data_base = Common::AlignUp(root_off + header_size, 0x20);

  // Assign file data offsets (each file aligned to 0x20).
  std::vector<u32> file_off(count, 0);
  u32 cur = data_base;
  for (u32 i = 0; i < count; ++i)
  {
    if (!nodes[i].is_dir)
    {
      cur = Common::AlignUp(cur, 0x20);
      file_off[i] = cur;
      cur += static_cast<u32>(nodes[i].data.size());
    }
  }
  const u32 total = Common::AlignUp(cur, 0x20);

  std::vector<u8> out(total, 0);
  WU32(out, 0, U8_MAGIC);
  WU32(out, 4, root_off);
  WU32(out, 8, header_size);
  WU32(out, 12, data_base);
  for (u32 i = 0; i < count; ++i)
  {
    const size_t no = root_off + size_t{i} * 12;
    out[no] = nodes[i].is_dir ? 1 : 0;
    out[no + 1] = static_cast<u8>(name_offsets[i] >> 16);
    out[no + 2] = static_cast<u8>(name_offsets[i] >> 8);
    out[no + 3] = static_cast<u8>(name_offsets[i]);
    if (nodes[i].is_dir)
    {
      WU32(out, no + 4, nodes[i].dir_parent);
      WU32(out, no + 8, nodes[i].dir_bound);
    }
    else
    {
      WU32(out, no + 4, file_off[i]);
      WU32(out, no + 8, static_cast<u32>(nodes[i].data.size()));
      std::copy(nodes[i].data.begin(), nodes[i].data.end(), out.begin() + file_off[i]);
    }
  }
  std::copy(strtab.begin(), strtab.end(), out.begin() + root_off + size_t{count} * 12);
  return out;
}

U8Node* FindFile(std::vector<U8Node>& nodes, std::string_view name)
{
  for (auto& n : nodes)
    if (!n.is_dir && n.name == name)
      return &n;
  return nullptr;
}

// ---------------------------------------------------------------------------
// TPL: locate the primary texture (format/dims/pixel region) inside a .tpl file.
// ---------------------------------------------------------------------------
struct TplTexture
{
  u32 format = 0, width = 0, height = 0;
  size_t data_offset = 0, data_size = 0;
};
std::optional<TplTexture> TplPrimaryTexture(const std::vector<u8>& tpl)
{
  if (!InBounds(tpl, 0, 12) || RU32(tpl, 0) != TPL_MAGIC)
    return std::nullopt;
  const u32 num = RU32(tpl, 4);
  const u32 table = RU32(tpl, 8);
  if (num == 0 || !InBounds(tpl, table, 8))
    return std::nullopt;
  const u32 img_hdr = RU32(tpl, table);  // texture 0's image header offset
  if (!InBounds(tpl, img_hdr, 0x10))
    return std::nullopt;
  TplTexture t;
  t.height = RU16(tpl, img_hdr);
  t.width = RU16(tpl, img_hdr + 2);
  t.format = RU32(tpl, img_hdr + 4);
  t.data_offset = RU32(tpl, img_hdr + 8);
  t.data_size = TextureDataSize(t.format, t.width, t.height);
  if (t.data_size == 0 || !InBounds(tpl, t.data_offset, t.data_size))
    return std::nullopt;
  return t;
}

// Largest .tpl file in an inner banner scene (timg/*.tpl). Returns its node name.
std::optional<std::string> LargestTpl(std::vector<U8Node>& scene)
{
  std::string best;
  size_t best_size = 0;
  for (auto& n : scene)
  {
    if (n.is_dir || n.name.size() < 4)
      continue;
    if (n.name.compare(n.name.size() - 4, 4, ".tpl") != 0)
      continue;
    if (n.data.size() > best_size)
    {
      best_size = n.data.size();
      best = n.name;
    }
  }
  if (best.empty())
    return std::nullopt;
  return best;
}

// A banner meta-file (icon.bin/banner.bin) opened down to its primary texture: the parsed
// inner scene (owned), the LZ wrapper it used, and the largest TPL's name + texture info.
struct BannerScene
{
  std::vector<U8Node> nodes;
  LzVariant variant;
  std::string tpl_name;
  TplTexture tex;
};

// Unwrap IMD5 -> decompress -> parse U8 -> locate the largest TPL and its primary texture.
// Returns the scene by value; re-resolve the node with FindFile(scene.nodes, scene.tpl_name)
// rather than holding a pointer (the vector is moved into the result).
std::optional<BannerScene> OpenBannerScene(const std::vector<u8>& file)
{
  const auto payload = Imd5Payload(file);
  if (!payload)
    return std::nullopt;
  LzVariant variant;
  const auto scene_bytes = LzDecompress(*payload, &variant);
  if (!scene_bytes)
    return std::nullopt;
  auto nodes = U8Parse(*scene_bytes);
  if (!nodes)
    return std::nullopt;
  const auto tpl_name = LargestTpl(*nodes);
  if (!tpl_name)
    return std::nullopt;
  const U8Node* node = FindFile(*nodes, *tpl_name);
  if (!node)
    return std::nullopt;
  const auto tex = TplPrimaryTexture(node->data);
  if (!tex)
    return std::nullopt;
  return BannerScene{std::move(*nodes), variant, *tpl_name, *tex};
}

// Re-host the game's primary texture art into one donor banner file (icon.bin or
// banner.bin). Keeps the donor scene byte-identical except the primary texture's pixels.
// Returns the rebuilt donor file bytes, or nullopt to leave the donor file unchanged.
std::optional<std::vector<u8>> SwapArtIntoFile(const std::vector<u8>& donor_file,
                                               const std::vector<u8>& game_file)
{
  std::optional<BannerScene> donor = OpenBannerScene(donor_file);
  if (!donor || !CanEncode(donor->tex.format))
    return std::nullopt;
  std::optional<BannerScene> game = OpenBannerScene(game_file);
  if (!game || !CanDecode(game->tex.format))
    return std::nullopt;

  // Decode the game's art (re-resolve the node from the owned scene -- no dangling pointer).
  const U8Node* game_tpl = FindFile(game->nodes, game->tpl_name);
  if (!game_tpl)
    return std::nullopt;
  const auto art = DecodeTexture(game->tex.format, game->tex.width, game->tex.height,
                                 game_tpl->data.data() + game->tex.data_offset, game->tex.data_size);
  if (!art)
    return std::nullopt;

  // Resize to the donor slot and encode into the donor's format; must match byte size.
  const Image resized = ResizeImage(*art, donor->tex.width, donor->tex.height);
  const std::vector<u8> encoded = EncodeTexture(donor->tex.format, resized);
  if (encoded.size() != donor->tex.data_size)
    return std::nullopt;

  // Overwrite the donor TPL pixels in place (size unchanged), repack, re-frame as all-literal
  // LZ10 in the donor's wrapper, and re-wrap IMD5.
  U8Node* donor_tpl = FindFile(donor->nodes, donor->tpl_name);
  if (!donor_tpl)
    return std::nullopt;
  std::copy(encoded.begin(), encoded.end(), donor_tpl->data.begin() + donor->tex.data_offset);
  const std::vector<u8> new_scene = U8Build(donor->nodes);
  return Imd5Wrap(LzCompressAllLiteral(new_scene, donor->variant));
}

// Patch the game's IMET title block (10 langs) into a banner, leaving everything else.
void PatchTitles(std::vector<u8>& banner, const std::vector<u8>& game_bnr)
{
  if (InBounds(game_bnr, 0x40, 4) && std::memcmp(game_bnr.data() + 0x40, "IMET", 4) == 0 &&
      InBounds(game_bnr, IMET_TITLES_OFF, IMET_TITLES_LEN) &&
      InBounds(banner, IMET_TITLES_OFF, IMET_TITLES_LEN))
  {
    std::copy(game_bnr.begin() + IMET_TITLES_OFF,
              game_bnr.begin() + IMET_TITLES_OFF + IMET_TITLES_LEN,
              banner.begin() + IMET_TITLES_OFF);
  }
}

// Recompute the IMET MD5 over [0, IMET_SIZE) with the 16-byte field zeroed (matches the
// known-good donor convention).
void FixImetMd5(std::vector<u8>& banner)
{
  if (!InBounds(banner, 0, IMET_SIZE))
    return;
  std::fill_n(banner.begin() + IMET_MD5_OFF, 16, u8{0});
  std::array<u8, 16> digest{};
  mbedtls_md5_ret(banner.data(), IMET_SIZE, digest.data());
  std::copy(digest.begin(), digest.end(), banner.begin() + IMET_MD5_OFF);
}

// Structural validation of a finished banner: IMET + outer U8 present, the three meta
// files exist, the IMET size fields agree with the U8 entries, and each compressed
// icon/banner re-parses (IMD5 -> LZ -> U8 -> TPL). Guards against shipping a brick.
bool Validate(const std::vector<u8>& banner)
{
  if (!InBounds(banner, 0x40, 4) || std::memcmp(banner.data() + 0x40, "IMET", 4) != 0)
    return false;
  if (banner.size() <= IMET_SIZE)
    return false;
  const std::vector<u8> outer(banner.begin() + IMET_SIZE, banner.end());
  auto nodes = U8Parse(outer);
  if (!nodes)
    return false;
  for (const auto& [name, size_off] : IMET_META_SIZE_FIELDS)
  {
    const U8Node* n = nullptr;
    for (const auto& node : *nodes)
      if (!node.is_dir && node.name == name)
        n = &node;
    if (!n)
      return false;
    if (RU32(banner, size_off) != n->data.size())  // IMET size field must match U8 entry
      return false;
    if (std::string_view(name) != "sound.bin")  // icon/banner must decode all the way down
    {
      const auto payload = Imd5Payload(n->data);
      if (!payload)
        return false;
      LzVariant v;
      const auto scene = LzDecompress(*payload, &v);
      if (!scene || !U8Parse(*scene))
        return false;
    }
  }
  return true;
}

// --- test fixtures: build a minimal-but-valid banner so the self-test can exercise the
// whole BuildArtChannelBanner pipeline in-memory (no real Wii assets needed) ---
std::vector<u8> MakeTestTpl(u32 format, u32 w, u32 h, u32 color)
{
  Image img;
  img.w = w;
  img.h = h;
  img.px.assign(size_t{w} * h, color);
  const std::vector<u8> pixels = EncodeTexture(format, img);
  std::vector<u8> tpl(0x40 + pixels.size(), 0);
  WU32(tpl, 0, TPL_MAGIC);
  WU32(tpl, 4, 1);     // one image
  WU32(tpl, 8, 0x0C);  // image table offset
  WU32(tpl, 0x0C, 0x14);
  WU32(tpl, 0x10, 0);  // no palette
  tpl[0x14] = static_cast<u8>(h >> 8);  // image header @0x14: height, width, format, data off
  tpl[0x15] = static_cast<u8>(h);
  tpl[0x16] = static_cast<u8>(w >> 8);
  tpl[0x17] = static_cast<u8>(w);
  WU32(tpl, 0x18, format);
  WU32(tpl, 0x1C, 0x40);
  std::copy(pixels.begin(), pixels.end(), tpl.begin() + 0x40);
  return tpl;
}

std::vector<u8> MakeTestBanner(u32 format, u32 w, u32 h, u32 color, u8 title_byte)
{
  const std::vector<u8> tpl = MakeTestTpl(format, w, h, color);
  std::vector<U8Node> scene;
  scene.push_back({true, "", 0, 2, {}});            // root dir, bound = node count
  scene.push_back({false, "main.tpl", 0, 0, tpl});  // the texture
  const std::vector<u8> icon = Imd5Wrap(LzCompressAllLiteral(U8Build(scene), LzVariant::Lz77Magic));
  const std::vector<u8> sound = Imd5Wrap(std::vector<u8>(64, 0xAA));

  std::vector<U8Node> outer;
  outer.push_back({true, "", 0, 5, {}});      // root, bound 5
  outer.push_back({true, "meta", 0, 5, {}});  // meta dir (parent root, bound 5)
  outer.push_back({false, "icon.bin", 0, 0, icon});
  outer.push_back({false, "banner.bin", 0, 0, icon});
  outer.push_back({false, "sound.bin", 0, 0, sound});
  const std::vector<u8> outer_arc = U8Build(outer);

  std::vector<u8> banner(IMET_SIZE, 0);
  std::memcpy(banner.data() + 0x40, "IMET", 4);
  WU32(banner, 0x48, 3);
  WU32(banner, 0x4C, static_cast<u32>(icon.size()));   // icon.bin size
  WU32(banner, 0x50, static_cast<u32>(icon.size()));   // banner.bin size
  WU32(banner, 0x54, static_cast<u32>(sound.size()));  // sound.bin size
  std::fill_n(banner.begin() + IMET_TITLES_OFF, IMET_TITLES_LEN, title_byte);
  banner.insert(banner.end(), outer_arc.begin(), outer_arc.end());
  return banner;
}
}  // namespace

std::optional<std::vector<u8>> BuildArtChannelBanner(const std::vector<u8>& donor_banner,
                                                     const std::vector<u8>& game_opening_bnr)
{
  if (!InBounds(donor_banner, 0x40, 4) || std::memcmp(donor_banner.data() + 0x40, "IMET", 4) != 0 ||
      donor_banner.size() <= IMET_SIZE)
  {
    return std::nullopt;  // donor itself unusable
  }

  // safe_base = donor scene + game titles. Always valid (this is the plain-donor fallback).
  std::vector<u8> safe_base = donor_banner;
  PatchTitles(safe_base, game_opening_bnr);
  FixImetMd5(safe_base);

  // Parse the donor's outer U8 and the game's outer U8.
  auto donor_outer = U8Parse({donor_banner.begin() + IMET_SIZE, donor_banner.end()});
  if (!donor_outer)
    return safe_base;
  const auto game_outer = U8Parse(InBounds(game_opening_bnr, IMET_SIZE, 1) ?
                                      std::vector<u8>(game_opening_bnr.begin() + IMET_SIZE,
                                                      game_opening_bnr.end()) :
                                      std::vector<u8>{});
  if (!game_outer)
    return safe_base;

  // Swap art into icon.bin and banner.bin independently (best-effort each).
  bool any_swapped = false;
  for (const char* fname : {"icon.bin", "banner.bin"})
  {
    U8Node* donor_file = FindFile(*donor_outer, fname);
    const U8Node* game_file = nullptr;
    for (const auto& n : *game_outer)
      if (!n.is_dir && n.name == fname)
        game_file = &n;
    if (!donor_file || !game_file)
      continue;
    if (auto rebuilt = SwapArtIntoFile(donor_file->data, game_file->data))
    {
      donor_file->data = std::move(*rebuilt);
      any_swapped = true;
    }
  }
  if (!any_swapped)
    return safe_base;

  // Reassemble: IMET (titles patched) + rebuilt outer U8, with IMET size fields updated.
  const std::vector<u8> new_outer = U8Build(*donor_outer);
  std::vector<u8> result(donor_banner.begin(), donor_banner.begin() + IMET_SIZE);
  PatchTitles(result, game_opening_bnr);
  for (const auto& [name, off] : IMET_META_SIZE_FIELDS)
  {
    if (const U8Node* n = FindFile(*donor_outer, name))
      WU32(result, off, static_cast<u32>(n->data.size()));
  }
  result.insert(result.end(), new_outer.begin(), new_outer.end());
  FixImetMd5(result);

  if (!Validate(result))
  {
    WARN_LOG_FMT(IOS_ES, "Forwarder: art banner failed validation; using plain donor");
    return safe_base;
  }
  return result;
}

bool RunSelfTests()
{
  // LZ10 round-trip (both wrappers): all-literal frame -> decompress -> identity.
  {
    std::vector<u8> raw(257);
    for (size_t i = 0; i < raw.size(); ++i)
      raw[i] = static_cast<u8>(i * 7 + 3);
    for (LzVariant v : {LzVariant::Lz77Magic, LzVariant::Lz10Bare})
    {
      LzVariant got;
      const auto back = LzDecompress(LzCompressAllLiteral(raw, v), &got);
      if (!back || *back != raw || got != v)
      {
        ERROR_LOG_FMT(IOS_ES, "Banner self-test: LZ10 round-trip failed");
        return false;
      }
    }
    // Uncompressed variant: the payload IS a (U8-magic-prefixed) archive, passed through.
    {
      std::vector<u8> u8raw{0x55, 0xAA, 0x38, 0x2D};  // U8_MAGIC, detected by LzDecompress
      u8raw.insert(u8raw.end(), raw.begin(), raw.end());
      LzVariant got;
      const auto back = LzDecompress(LzCompressAllLiteral(u8raw, LzVariant::Uncompressed), &got);
      if (!back || *back != u8raw || got != LzVariant::Uncompressed)
      {
        ERROR_LOG_FMT(IOS_ES, "Banner self-test: LZ uncompressed round-trip failed");
        return false;
      }
    }
  }
  // IMD5 wrap -> unwrap identity.
  {
    const std::vector<u8> payload(100, 0xAB);
    const auto back = Imd5Payload(Imd5Wrap(payload));
    if (!back || *back != payload)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: IMD5 round-trip failed");
      return false;
    }
  }
  // U8 build -> parse identity (a dir with two files, like meta/{a,b}).
  {
    std::vector<U8Node> in;
    in.push_back({true, "", 0, 4, {}});               // root (bound = node count)
    in.push_back({true, "meta", 0, 4, {}});           // dir, parent root, bound 4
    in.push_back({false, "a.tpl", 0, 0, {1, 2, 3}});  // file
    in.push_back({false, "b.bin", 0, 0, {9, 8, 7, 6}});
    const auto back = U8Parse(U8Build(in));
    if (!back || back->size() != 4 || (*back)[2].name != "a.tpl" ||
        (*back)[2].data != std::vector<u8>{1, 2, 3} || (*back)[3].data != std::vector<u8>{9, 8, 7, 6})
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: U8 round-trip failed");
      return false;
    }
  }
  // RGBA8 is 8-bit/channel -> encode -> decode must be exact.
  {
    Image img;
    img.w = 6;
    img.h = 5;
    img.px.resize(30);
    for (size_t i = 0; i < img.px.size(); ++i)
      img.px[i] = (u32{static_cast<u8>(i * 11)} << 24) | (u32{static_cast<u8>(i * 5)} << 16) |
                  (u32{static_cast<u8>(i * 3)} << 8) | static_cast<u8>(i * 2);
    const auto enc = EncodeTexture(FMT_RGBA8, img);
    const auto dec = DecodeTexture(FMT_RGBA8, img.w, img.h, enc.data(), enc.size());
    if (!dec || dec->px != img.px)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: RGBA8 round-trip failed");
      return false;
    }
  }
  // RGB5A3 is lossy, but the encoded size must match the formula and decode back to dims.
  {
    Image img;
    img.w = 8;
    img.h = 8;
    img.px.assign(64, 0xFF8040C0);
    const auto enc = EncodeTexture(FMT_RGB5A3, img);
    const auto dec = DecodeTexture(FMT_RGB5A3, img.w, img.h, enc.data(), enc.size());
    if (enc.size() != TextureDataSize(FMT_RGB5A3, 8, 8) || !dec || dec->w != 8 || dec->h != 8)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: RGB5A3 encode failed");
      return false;
    }
  }
  // RGB5A3 must preserve translucency (regression guard: a translucent pixel must NOT decode
  // back as fully opaque, which was the old Common::Decode5A3Image behavior).
  {
    Image img;
    img.w = 4;
    img.h = 4;
    img.px.assign(16, 0x40C0A080u);  // alpha 0x40 -> 4443 path
    const auto enc = EncodeTexture(FMT_RGB5A3, img);
    const auto dec = DecodeTexture(FMT_RGB5A3, 4, 4, enc.data(), enc.size());
    if (!dec || ((dec->px[0] >> 24) & 0xFF) >= 0xFF)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: RGB5A3 translucency not preserved");
      return false;
    }
  }
  // RGB565 decode (hand-built bytes, since we don't encode RGB565): a solid 4x4 tile.
  {
    std::vector<u8> data(TextureDataSize(FMT_RGB565, 4, 4), 0);  // 32 bytes
    const u16 px = static_cast<u16>((0x18 << 11) | (0x30 << 5) | 0x10);
    for (size_t i = 0; i + 1 < data.size(); i += 2)
    {
      data[i] = static_cast<u8>(px >> 8);
      data[i + 1] = static_cast<u8>(px);
    }
    const auto dec = DecodeTexture(FMT_RGB565, 4, 4, data.data(), data.size());
    const u32 expect = 0xFF000000u | (Conv5To8(0x18) << 16) | (Conv6To8(0x30) << 8) | Conv5To8(0x10);
    if (!dec || dec->w != 4 || dec->h != 4 || dec->px[0] != expect)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: RGB565 decode failed");
      return false;
    }
  }
  // End-to-end: a fake RGBA8 donor + a fake RGB5A3 game, run the whole pipeline, and prove
  // the result carries the GAME's art in the DONOR's texture slot (not a silent fallback).
  {
    const u32 donor_color = 0xFF204060;  // 0xAARRGGBB
    const u32 game_color = 0xFFC0A080;
    const std::vector<u8> donor = MakeTestBanner(FMT_RGBA8, 32, 32, donor_color, 0x11);
    const std::vector<u8> game = MakeTestBanner(FMT_RGB5A3, 48, 48, game_color, 0x22);
    const auto result = BuildArtChannelBanner(donor, game);
    if (!result || result->size() <= IMET_SIZE ||
        (*result)[IMET_TITLES_OFF] != 0x22)  // titles from game
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: e2e build/titles failed");
      return false;
    }
    const std::vector<u8> outer(result->begin() + IMET_SIZE, result->end());
    auto nodes = U8Parse(outer);
    U8Node* icon = nodes ? FindFile(*nodes, "icon.bin") : nullptr;
    if (!icon || RU32(*result, 0x4C) != icon->data.size())  // IMET icon size fixed up
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: e2e outer/IMET-size failed");
      return false;
    }
    const auto payload = Imd5Payload(icon->data);
    LzVariant v;
    const auto scene_bytes = payload ? LzDecompress(*payload, &v) : std::nullopt;
    auto scene = scene_bytes ? U8Parse(*scene_bytes) : std::nullopt;
    const auto tpl_name = scene ? LargestTpl(*scene) : std::nullopt;
    U8Node* tnode = tpl_name ? FindFile(*scene, *tpl_name) : nullptr;
    const auto tex = tnode ? TplPrimaryTexture(tnode->data) : std::nullopt;
    if (!tex || tex->format != FMT_RGBA8 || tex->width != 32 || tex->height != 32)
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: e2e donor slot not preserved");
      return false;
    }
    const auto img = DecodeTexture(tex->format, tex->width, tex->height,
                                   tnode->data.data() + tex->data_offset, tex->data_size);
    const u32 c = img ? img->px[size_t{16} * 32 + 16] : 0;  // center pixel
    const auto near = [](u32 a, u32 b) { return (a > b ? a - b : b - a) <= 24; };
    if (!img || !near((c >> 16) & 0xFF, 0xC0) || !near((c >> 8) & 0xFF, 0xA0) ||
        !near(c & 0xFF, 0x80))
    {
      ERROR_LOG_FMT(IOS_ES, "Banner self-test: e2e art swap not applied (fell back to donor)");
      return false;
    }
  }
  return true;
}
}  // namespace WiiUtils::Banner
