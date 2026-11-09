/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Fonts of our gtests: the bundled files as the engine ships them (GRE/fonts), and a copy
// of one with its sfnt directory rebuilt around tables of the test's own choosing.

#ifndef STEALTH_TEST_FONTS_H
#define STEALTH_TEST_FONTS_H

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

#ifndef STEALTH_TEST_FONTS_STANDALONE  // set by the check program outside Firefox
#  include "nsCOMPtr.h"
#  include "nsDirectoryServiceDefs.h"
#  include "nsDirectoryServiceUtils.h"
#  include "nsIFile.h"
#  include "nsString.h"
#  include "nsTArray.h"
#  include "prio.h"
#endif

namespace stealth_test_fonts {

#ifndef STEALTH_TEST_FONTS_STANDALONE
// The bundled font, as the engine ships it (GRE/fonts). Empty when it is not there.
inline nsTArray<uint8_t> ReadBundledFont(const char* aName) {
  nsTArray<uint8_t> out;
  nsCOMPtr<nsIFile> file;
  int64_t size = 0;
  PRFileDesc* fd = nullptr;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(file))) ||
      NS_FAILED(file->Append(u"fonts"_ns)) ||
      NS_FAILED(file->Append(NS_ConvertUTF8toUTF16(aName))) ||
      NS_FAILED(file->GetFileSize(&size)) ||
      NS_FAILED(file->OpenNSPRFileDesc(PR_RDONLY, 0, &fd))) {
    return out;
  }
  out.SetLength(size_t(size));
  const int32_t read = PR_Read(fd, out.Elements(), int32_t(size));
  PR_Close(fd);
  if (read != int32_t(size)) {
    out.Clear();
  }
  return out;
}
#endif

constexpr uint32_t SfntTag(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 |
         uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

struct SfntTable {
  uint32_t mTag;
  std::vector<uint8_t> mData;
};

namespace detail {

inline uint16_t Be16(const uint8_t* aData) {
  return uint16_t(aData[0] << 8 | aData[1]);
}
inline uint32_t Be32(const uint8_t* aData) {
  return uint32_t(aData[0]) << 24 | uint32_t(aData[1]) << 16 |
         uint32_t(aData[2]) << 8 | uint32_t(aData[3]);
}
inline void Put16(std::vector<uint8_t>& aOut, uint32_t aValue) {
  aOut.push_back(uint8_t(aValue >> 8));
  aOut.push_back(uint8_t(aValue));
}
inline void Put32(std::vector<uint8_t>& aOut, uint32_t aValue) {
  Put16(aOut, aValue >> 16);
  Put16(aOut, aValue & 0xffff);
}

// The sfnt table checksum: the big endian words of the table, zero padded to 4 bytes.
inline uint32_t Checksum(const std::vector<uint8_t>& aData) {
  uint32_t sum = 0;
  for (size_t i = 0; i < aData.size(); i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4; j++) {
      word = word << 8 | (i + j < aData.size() ? aData[i + j] : 0);
    }
    sum += word;
  }
  return sum;
}

}  // namespace detail

// The font file `aFont` (a single sfnt: TrueType or OpenType, not a collection) with the
// tables of `aTables` added, or in place of those it has with the same tag. The directory is
// rebuilt: tags sorted, searchRange, entrySelector and rangeShift, checksums, offsets, every
// table padded to 4 bytes. head.checkSumAdjustment is left as it was (readers do not check
// it). Empty when the input is not an sfnt this can read.
inline std::vector<uint8_t> WithTables(const uint8_t* aFont, size_t aLength,
                                       const std::vector<SfntTable>& aTables) {
  using namespace detail;
  if (!aFont || aLength < 12) {
    return {};
  }
  const uint32_t version = Be32(aFont);
  if (version != 0x00010000 && version != SfntTag('O', 'T', 'T', 'O') &&
      version != SfntTag('t', 'r', 'u', 'e')) {
    return {};
  }
  const uint32_t count = Be16(aFont + 4);
  if (aLength < 12 + size_t(count) * 16) {
    return {};
  }
  std::vector<SfntTable> tables;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* record = aFont + 12 + size_t(i) * 16;
    const uint32_t tag = Be32(record);
    const size_t offset = Be32(record + 8);
    const size_t length = Be32(record + 12);
    if (offset > aLength || length > aLength - offset) {
      return {};
    }
    const bool replaced =
        std::any_of(aTables.begin(), aTables.end(),
                    [tag](const SfntTable& t) { return t.mTag == tag; });
    if (!replaced) {
      tables.push_back({tag, std::vector<uint8_t>(aFont + offset, aFont + offset + length)});
    }
  }
  tables.insert(tables.end(), aTables.begin(), aTables.end());
  std::sort(tables.begin(), tables.end(),
            [](const SfntTable& a, const SfntTable& b) { return a.mTag < b.mTag; });

  const uint32_t n = uint32_t(tables.size());
  uint32_t entrySelector = 0;
  while ((2u << entrySelector) <= n) {
    entrySelector++;
  }
  const uint32_t searchRange = 16u << entrySelector;

  std::vector<uint8_t> out;
  Put32(out, version);
  Put16(out, n);
  Put16(out, searchRange);
  Put16(out, entrySelector);
  Put16(out, n * 16 - searchRange);
  uint32_t offset = 12 + 16 * n;
  for (const SfntTable& t : tables) {
    Put32(out, t.mTag);
    Put32(out, Checksum(t.mData));
    Put32(out, offset);
    Put32(out, uint32_t(t.mData.size()));
    offset += uint32_t((t.mData.size() + 3) & ~size_t(3));
  }
  for (const SfntTable& t : tables) {
    out.insert(out.end(), t.mData.begin(), t.mData.end());
    out.resize((out.size() + 3) & ~size_t(3), 0);
  }
  return out;
}

// A COLR (version 0) and a CPAL of one palette of one colour, with a base glyph of one layer
// whose palette index is `aLayerPalette`. Index 0 is a font DirectWrite takes; any other than
// 0xFFFF is past numPaletteEntries (1), and ColrBuilder refuses the face (CreateFontFace
// fails): idwr_font_check_color_tables returns E_FILEFORMAT. Both tables are well formed to
// FreeType, which reads the palette index only when a colour layer is asked for.
inline std::vector<SfntTable> ColorTables(uint16_t aLayerPalette) {
  using namespace detail;
  std::vector<uint8_t> colr;
  Put16(colr, 0);   // version
  Put16(colr, 1);   // numBaseGlyphRecords
  Put32(colr, 14);  // baseGlyphRecordsOffset
  Put32(colr, 20);  // layerRecordsOffset
  Put16(colr, 1);   // numLayerRecords
  Put16(colr, 36);  // base glyph record: glyphID (A in Arial)
  Put16(colr, 0);   //   firstLayerIndex
  Put16(colr, 1);   //   numLayers
  Put16(colr, 37);  // layer record: glyphID
  Put16(colr, aLayerPalette);

  std::vector<uint8_t> cpal;
  Put16(cpal, 0);   // version
  Put16(cpal, 1);   // numPaletteEntries
  Put16(cpal, 1);   // numPalettes
  Put16(cpal, 1);   // numColorRecords
  Put32(cpal, 14);  // colorRecordsArrayOffset
  Put16(cpal, 0);   // colorRecordIndices[0]
  Put32(cpal, 0xff0000ff);  // the colour record, B G R A

  return {{SfntTag('C', 'O', 'L', 'R'), colr}, {SfntTag('C', 'P', 'A', 'L'), cpal}};
}

}  // namespace stealth_test_fonts

#endif  // STEALTH_TEST_FONTS_H
