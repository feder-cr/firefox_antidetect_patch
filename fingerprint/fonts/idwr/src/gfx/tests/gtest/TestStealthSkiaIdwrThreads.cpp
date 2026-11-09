/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Skia locks per strike, not per typeface: two sizes of one typeface are two strikes,
// and two threads draw them at once. The idwr font under a typeface carries lazy caches
// (the CFF metrics rasterizer) that calls taking it as const write, so the typeface has
// one lock for it (IdwrFontHolder::fMutex). DirectWrite is thread safe and the original
// has no lock, which is why only this port needs one.
//
// Four threads draw the glyphs of four sizes of ONE typeface, one size per thread, from
// the typeface's very first call (the lazy caches empty), and every mask and advance must
// be the bytes of the sequential run. Without the lock the caches are written by two
// threads at once: ThreadSanitizer reports it, and the bytes can differ.
//
// The font is a CFF font made here, glyph by glyph from a fixed generator: the bundle has
// no OpenType-CFF font, and one is not copied into the tree for a test. It is small, but
// it takes the same path in idwr as any CFF font: the CFF table's charstrings, the metrics
// from hmtx, and the rasterizer.

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "skia/include/core/SkBitmap.h"
#include "skia/include/core/SkCanvas.h"
#include "skia/include/core/SkData.h"
#include "skia/include/core/SkFont.h"
#include "skia/include/core/SkFontStyle.h"
#include "skia/include/core/SkGraphics.h"
#include "skia/include/core/SkPaint.h"
#include "skia/include/core/SkTypeface.h"
#include "skia/include/ports/SkTypeface_idwr.h"

namespace {

constexpr int kGlyphCount = 64;  // glyph 0 is .notdef

class Bytes {
 public:
  void U8(uint32_t aValue) { mData.push_back(uint8_t(aValue)); }
  void U16(uint32_t aValue) {
    U8(aValue >> 8);
    U8(aValue);
  }
  void U32(uint32_t aValue) {
    U16(aValue >> 16);
    U16(aValue);
  }
  void Tag(const char* aTag) {
    for (int i = 0; i < 4; ++i) {
      U8(uint8_t(aTag[i]));
    }
  }
  void Append(const Bytes& aOther) {
    mData.insert(mData.end(), aOther.mData.begin(), aOther.mData.end());
  }
  void Pad4() {
    while (mData.size() % 4) {
      U8(0);
    }
  }
  size_t Size() const { return mData.size(); }
  const std::vector<uint8_t>& Data() const { return mData; }

 private:
  std::vector<uint8_t> mData;
};

// A CFF DICT operand as a 32 bit integer: five bytes, so that the offsets of a DICT do not
// depend on the values it holds.
void DictInt(Bytes& aOut, int32_t aValue) {
  aOut.U8(29);
  aOut.U32(uint32_t(aValue));
}

// A Type 2 charstring number as a 16 bit integer (28).
void CharInt(Bytes& aOut, int aValue) {
  aOut.U8(28);
  aOut.U16(uint32_t(aValue) & 0xffff);
}

// The outline of one glyph: one or two contours of lines and curves from a fixed generator,
// each closed by the next rmoveto or by endchar. The numbers stay within a few hundred
// units, so the glyphs are inked at every size the test draws.
Bytes GlyphCharString(int aGlyph) {
  Bytes cs;
  if (aGlyph == 0) {
    cs.U8(14);  // endchar
    return cs;
  }
  uint32_t state = uint32_t(aGlyph) * 2654435761u + 12345u;
  auto next = [&state](int aRange) {
    state = state * 1664525u + 1013904223u;
    return int((state >> 8) % uint32_t(2 * aRange + 1)) - aRange;
  };
  const int contours = 1 + (aGlyph % 2);
  for (int c = 0; c < contours; ++c) {
    CharInt(cs, c == 0 ? 60 + next(40) : next(150));
    CharInt(cs, c == 0 ? 10 + next(10) : 180 + next(60));
    cs.U8(21);  // rmoveto
    const int segments = 3 + (aGlyph + c) % 3;
    for (int s = 0; s < segments; ++s) {
      if ((aGlyph + s) % 2) {
        for (int i = 0; i < 2; ++i) {
          CharInt(cs, next(180));
        }
        cs.U8(5);  // rlineto
      } else {
        for (int i = 0; i < 6; ++i) {
          CharInt(cs, next(110));
        }
        cs.U8(8);  // rrcurveto
      }
    }
  }
  cs.U8(14);  // endchar
  return cs;
}

// An INDEX of the given objects, offsets of two bytes.
Bytes Index(const std::vector<Bytes>& aObjects) {
  Bytes out;
  out.U16(uint32_t(aObjects.size()));
  if (aObjects.empty()) {
    return out;
  }
  out.U8(2);
  uint32_t offset = 1;
  out.U16(offset);
  for (const Bytes& object : aObjects) {
    offset += uint32_t(object.Size());
    out.U16(offset);
  }
  for (const Bytes& object : aObjects) {
    out.Append(object);
  }
  return out;
}

Bytes CffTable() {
  Bytes nameData;
  for (char ch : {'S', 'y', 'n', 't', 'h'}) {
    nameData.U8(uint8_t(ch));
  }
  Bytes nameIndex = Index({nameData});

  std::vector<Bytes> charStrings;
  for (int g = 0; g < kGlyphCount; ++g) {
    charStrings.push_back(GlyphCharString(g));
  }
  Bytes charStringsIndex = Index(charStrings);

  // defaultWidthX 600, nominalWidthX 0.
  Bytes priv;
  DictInt(priv, 600);
  priv.U8(20);
  DictInt(priv, 0);
  priv.U8(21);

  // The top DICT is 38 bytes whatever the offsets: header 4, name INDEX, top DICT INDEX
  // (2 + 1 + 4 + 38), string INDEX 2, global subr INDEX 2, then the charstrings.
  constexpr size_t kTopDictSize = 21 + 6 + 11;
  const size_t charStringsOffset =
      4 + nameIndex.Size() + (2 + 1 + 4 + kTopDictSize) + 2 + 2;
  const size_t privateOffset = charStringsOffset + charStringsIndex.Size();

  Bytes top;
  for (int v : {0, -200, 900, 900}) {
    DictInt(top, v);
  }
  top.U8(5);  // FontBBox
  DictInt(top, int32_t(charStringsOffset));
  top.U8(17);  // CharStrings
  DictInt(top, int32_t(priv.Size()));
  DictInt(top, int32_t(privateOffset));
  top.U8(18);  // Private
  Bytes topIndex = Index({top});

  Bytes cff;
  cff.U8(1);  // major
  cff.U8(0);  // minor
  cff.U8(4);  // hdrSize
  cff.U8(1);  // offSize
  cff.Append(nameIndex);
  cff.Append(topIndex);
  cff.Append(Index({}));  // strings
  cff.Append(Index({}));  // global subrs
  cff.Append(charStringsIndex);
  cff.Append(priv);
  return cff;
}

uint32_t Checksum(const Bytes& aTable) {
  uint32_t sum = 0;
  const std::vector<uint8_t>& d = aTable.Data();
  for (size_t i = 0; i < d.size(); i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4; ++j) {
      word = (word << 8) | (i + j < d.size() ? d[i + j] : 0);
    }
    sum += word;
  }
  return sum;
}

// An OpenType font with CFF outlines: CFF, OS/2, head, hhea, hmtx, maxp, name, post.
std::vector<uint8_t> MakeCffFont() {
  Bytes os2;  // version 3
  os2.U16(3);
  os2.U16(500);  // xAvgCharWidth
  os2.U16(400);  // usWeightClass
  os2.U16(5);    // usWidthClass
  for (int i = 0; i < 12; ++i) {
    os2.U16(0);  // fsType .. sFamilyClass
  }
  for (int i = 0; i < 10; ++i) {
    os2.U8(0);  // panose
  }
  for (int i = 0; i < 4; ++i) {
    os2.U32(0);  // ulUnicodeRange
  }
  os2.Tag("SYNT");
  os2.U16(0x40);  // fsSelection: regular
  os2.U16(0x20);  // usFirstCharIndex
  os2.U16(0x7e);  // usLastCharIndex
  os2.U16(800);   // sTypoAscender
  os2.U16(uint16_t(-200));
  os2.U16(0);      // sTypoLineGap
  os2.U16(900);    // usWinAscent
  os2.U16(200);    // usWinDescent
  os2.U32(1);      // ulCodePageRange1
  os2.U32(0);      // ulCodePageRange2
  os2.U16(500);    // sxHeight
  os2.U16(700);    // sCapHeight
  os2.U16(0);      // usDefaultChar
  os2.U16(0x20);   // usBreakChar
  os2.U16(1);      // usMaxContext

  Bytes head;
  head.U32(0x00010000);
  head.U32(0x00010000);  // fontRevision
  head.U32(0);           // checkSumAdjustment
  head.U32(0x5f0f3cf5);  // magic
  head.U16(0x000b);      // flags
  head.U16(1000);        // unitsPerEm
  head.U32(0);
  head.U32(0);  // created
  head.U32(0);
  head.U32(0);  // modified
  head.U16(0);
  head.U16(uint16_t(-200));
  head.U16(900);
  head.U16(900);  // bounding box
  head.U16(0);    // macStyle
  head.U16(8);    // lowestRecPPEM
  head.U16(2);    // fontDirectionHint
  head.U16(0);    // indexToLocFormat
  head.U16(0);    // glyphDataFormat

  Bytes hhea;
  hhea.U32(0x00010000);
  hhea.U16(800);
  hhea.U16(uint16_t(-200));
  hhea.U16(0);    // lineGap
  hhea.U16(800);  // advanceWidthMax
  hhea.U16(0);    // minLeftSideBearing
  hhea.U16(0);    // minRightSideBearing
  hhea.U16(900);  // xMaxExtent
  hhea.U16(1);    // caretSlopeRise
  hhea.U16(0);    // caretSlopeRun
  hhea.U16(0);    // caretOffset
  for (int i = 0; i < 4; ++i) {
    hhea.U16(0);
  }
  hhea.U16(0);  // metricDataFormat
  hhea.U16(kGlyphCount);

  Bytes hmtx;
  for (int g = 0; g < kGlyphCount; ++g) {
    hmtx.U16(500 + (g * 7) % 300);
    hmtx.U16(0);
  }

  Bytes maxp;
  maxp.U32(0x00005000);
  maxp.U16(kGlyphCount);

  Bytes name;  // no records
  name.U16(0);
  name.U16(0);
  name.U16(6);

  Bytes post;  // version 3
  post.U32(0x00030000);
  for (int i = 0; i < 7; ++i) {
    post.U32(0);
  }

  struct Table {
    const char* tag;
    Bytes data;
  };
  const Table tables[] = {{"CFF ", CffTable()}, {"OS/2", os2},   {"head", head},
                          {"hhea", hhea},       {"hmtx", hmtx},   {"maxp", maxp},
                          {"name", name},       {"post", post}};
  constexpr uint32_t kTableCount = sizeof(tables) / sizeof(tables[0]);

  Bytes font;
  font.Tag("OTTO");
  font.U16(kTableCount);
  font.U16(128);  // searchRange: 8 tables
  font.U16(3);    // entrySelector
  font.U16(0);    // rangeShift
  uint32_t offset = 12 + 16 * kTableCount;
  for (const Table& table : tables) {
    font.Tag(table.tag);
    font.U32(Checksum(table.data));
    font.U32(offset);
    font.U32(uint32_t(table.data.Size()));
    offset += uint32_t((table.data.Size() + 3) & ~size_t(3));
  }
  for (const Table& table : tables) {
    font.Append(table.data);
    font.Pad4();
  }
  return font.Data();
}

uint64_t Fnv1a(const void* aData, size_t aLength, uint64_t aHash) {
  const uint8_t* d = static_cast<const uint8_t*>(aData);
  for (size_t i = 0; i < aLength; ++i) {
    aHash = (aHash ^ d[i]) * 0x100000001b3ULL;
  }
  return aHash;
}

struct Drawn {
  uint64_t mHash = 0xcbf29ce484222325ULL;
  uint64_t mInk = 0;  // pixels that are not zero
};

constexpr int kGlyphsDrawn = 60;
constexpr int kFirstGlyph = 3;
constexpr int kSide = 96;
constexpr int kThreads = 4;
constexpr float kSizes[kThreads] = {9.5f, 12.0f, 15.5f, 21.0f};

// The advances and the A8 masks of kGlyphsDrawn glyphs at one size: one strike.
Drawn Draw(const sk_sp<SkTypeface>& aTypeface, float aSize) {
  SkFont font(aTypeface, aSize);
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setSubpixel(true);
  Drawn drawn;
  for (int g = 0; g < kGlyphsDrawn; ++g) {
    uint16_t id = uint16_t(kFirstGlyph + g);
    SkScalar width = 0;
    font.getWidths(SkSpan<const SkGlyphID>(&id, 1), SkSpan<SkScalar>(&width, 1));
    drawn.mHash = Fnv1a(&width, sizeof(width), drawn.mHash);

    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeA8(kSide, kSide));
    bitmap.eraseColor(0);
    SkCanvas canvas(bitmap);
    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    canvas.drawSimpleText(&id, sizeof(id), SkTextEncoding::kGlyphID, 20.25f, 60.0f,
                          font, paint);
    for (int y = 0; y < kSide; ++y) {
      const uint8_t* row = bitmap.getAddr8(0, y);
      drawn.mHash = Fnv1a(row, kSide, drawn.mHash);
      for (int x = 0; x < kSide; ++x) {
        drawn.mInk += row[x] != 0;
      }
    }
  }
  return drawn;
}

// A new typeface over the font: its idwr font has empty lazy caches.
sk_sp<SkTypeface> MakeTypeface(const sk_sp<SkData>& aBytes) {
  return sk_sp<SkTypeface>(SkCreateTypefaceFromIdwr(
      aBytes, 0, 0, nullptr, 0, SkFontStyle(), /* renderingMode */ 0,
      /* gamma */ 1.0f, /* contrast */ 0.0f, /* clearTypeLevel */ 1.0f));
}

}  // namespace

TEST(StealthSkiaIdwrThreads, FourStrikesOfOneTypefaceOnFourThreadsDrawTheSequentialRun)
{
  const std::vector<uint8_t> fontBytes = MakeCffFont();
  sk_sp<SkData> bytes = SkData::MakeWithCopy(fontBytes.data(), fontBytes.size());
  ASSERT_TRUE(bytes);

  // One strike after the other, each on a typeface of its own with the caches empty.
  Drawn expected[kThreads];
  for (int i = 0; i < kThreads; ++i) {
    SkGraphics::PurgeFontCache();
    sk_sp<SkTypeface> typeface = MakeTypeface(bytes);
    ASSERT_TRUE(typeface) << "idwr takes no typeface of the CFF font made here";
    expected[i] = Draw(typeface, kSizes[i]);
    // The test is not vacuous: glyphs are drawn.
    ASSERT_GT(expected[i].mInk, uint64_t(kGlyphsDrawn)) << "size " << kSizes[i];
  }

  constexpr int kRounds = 40;
  int differing = 0;
  for (int round = 0; round < kRounds; ++round) {
    SkGraphics::PurgeFontCache();
    sk_sp<SkTypeface> typeface = MakeTypeface(bytes);
    ASSERT_TRUE(typeface);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    Drawn got[kThreads];
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        ready.fetch_add(1);
        while (!go.load()) {
          std::this_thread::yield();
        }
        got[t] = Draw(typeface, kSizes[t]);
      });
    }
    while (ready.load() < kThreads) {
      std::this_thread::yield();
    }
    go.store(true);
    for (std::thread& thread : threads) {
      thread.join();
    }
    for (int t = 0; t < kThreads; ++t) {
      if (got[t].mHash != expected[t].mHash || got[t].mInk != expected[t].mInk) {
        ++differing;
      }
    }
  }
  EXPECT_EQ(differing, 0) << differing << " of " << kRounds * kThreads
                          << " runs differ from the sequential run";
}
