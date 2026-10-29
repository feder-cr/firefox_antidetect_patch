/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The fontconfig half of the bundle-only font list (Linux only): the helpers
// take an FcPattern*, so no browser window is needed. Each case asserts the
// behaviour the code had inline in gfxFcPlatformFontList.cpp before it moved
// into StealthFcFontList.

#include "gtest/gtest.h"

#include <fontconfig/fontconfig.h>

#include <string>
#include <utility>

#include "StealthFcFontList.h"
#include "mozilla/Preferences.h"

using mozilla::Preferences;

namespace {

const char* const kFcPrefs[] = {
    "zoom.stealth.text.freetype_hintstyle",
    "zoom.stealth.text.freetype_antialias",
};

// Back to stock Firefox (-1, nothing declared), whatever the test did.
struct RestoreFcStock {
  ~RestoreFcStock() {
    for (const char* name : kFcPrefs) {
      Preferences::ClearUser(name);
    }
  }
};

struct ScopedFcPattern {
  ScopedFcPattern() : mPattern(FcPatternCreate()) {}
  ~ScopedFcPattern() { FcPatternDestroy(mPattern); }
  FcPattern* mPattern;
};

}  // namespace

TEST(StealthFcFontList, DeclareFontFile)
{
  ScopedFcPattern p;
  StealthDeclareFontFile(p.mPattern, nullptr, 3);
  StealthDeclareFontFile(p.mPattern, "", 3);
  FcChar8* file = nullptr;
  int index = -1;
  EXPECT_EQ(FcResultNoMatch,
            FcPatternGetString(p.mPattern, FC_FILE, 0, &file));
  EXPECT_EQ(FcResultNoMatch,
            FcPatternGetInteger(p.mPattern, FC_INDEX, 0, &index));

  StealthDeclareFontFile(p.mPattern, "/x/a.ttc", 2);
  ASSERT_EQ(FcResultMatch, FcPatternGetString(p.mPattern, FC_FILE, 0, &file));
  EXPECT_STREQ("/x/a.ttc", reinterpret_cast<const char*>(file));
  ASSERT_EQ(FcResultMatch,
            FcPatternGetInteger(p.mPattern, FC_INDEX, 0, &index));
  EXPECT_EQ(2, index);
}

TEST(StealthFcFontList, DeclaredFontFile)
{
  std::string file;
  int index = 0;
  {
    // A data face with no file in the pattern (a page's font): nothing is set.
    ScopedFcPattern p;
    StealthDeclaredFontFile(p.mPattern, file, index);
    EXPECT_TRUE(file.empty());
    EXPECT_EQ(0, index);
  }
  {
    // A data face whose pattern names its file (the bundle): file and index.
    ScopedFcPattern p;
    FcPatternAddString(p.mPattern, FC_FILE,
                       reinterpret_cast<const FcChar8*>("/x/b.ttf"));
    FcPatternAddInteger(p.mPattern, FC_INDEX, 5);
    StealthDeclaredFontFile(p.mPattern, file, index);
    EXPECT_EQ("/x/b.ttf", file);
    EXPECT_EQ(5, index);
  }
  {
    // Mozilla's block already wrote the file (a normal face): left as it is.
    ScopedFcPattern p;
    FcPatternAddString(p.mPattern, FC_FILE,
                       reinterpret_cast<const FcChar8*>("/x/other.ttf"));
    FcPatternAddInteger(p.mPattern, FC_INDEX, 9);
    StealthDeclaredFontFile(p.mPattern, file, index);
    EXPECT_EQ("/x/b.ttf", file);
    EXPECT_EQ(5, index);
  }
  {
    // A file but no index: the pattern does not name a file.
    ScopedFcPattern p;
    FcPatternAddString(p.mPattern, FC_FILE,
                       reinterpret_cast<const FcChar8*>("/x/c.ttf"));
    file.clear();
    index = 0;
    StealthDeclaredFontFile(p.mPattern, file, index);
    EXPECT_TRUE(file.empty());
    EXPECT_EQ(0, index);
  }
}

TEST(StealthFcFontList, TextOptions)
{
  RestoreFcStock restore;
  FcBool hinting = FcFalse;
  int hintstyle = -1;
  FcBool antialias = FcFalse;

  Preferences::SetInt("zoom.stealth.text.freetype_hintstyle", FC_HINT_NONE);
  {
    ScopedFcPattern p;
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetInteger(p.mPattern, FC_HINT_STYLE, 0, &hintstyle));
    EXPECT_EQ(FC_HINT_NONE, hintstyle);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetBool(p.mPattern, FC_HINTING, 0, &hinting));
    EXPECT_EQ(FcFalse, hinting);
  }
  Preferences::SetInt("zoom.stealth.text.freetype_hintstyle", FC_HINT_SLIGHT);
  {
    // The declaration replaces what the host configuration put there.
    ScopedFcPattern p;
    FcPatternAddInteger(p.mPattern, FC_HINT_STYLE, FC_HINT_FULL);
    FcPatternAddBool(p.mPattern, FC_HINTING, FcFalse);
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetInteger(p.mPattern, FC_HINT_STYLE, 0, &hintstyle));
    EXPECT_EQ(FC_HINT_SLIGHT, hintstyle);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetBool(p.mPattern, FC_HINTING, 0, &hinting));
    EXPECT_EQ(FcTrue, hinting);
  }
  Preferences::SetInt("zoom.stealth.text.freetype_hintstyle", -1);
  {
    // Not declared: the host value stays.
    ScopedFcPattern p;
    FcPatternAddInteger(p.mPattern, FC_HINT_STYLE, FC_HINT_FULL);
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetInteger(p.mPattern, FC_HINT_STYLE, 0, &hintstyle));
    EXPECT_EQ(FC_HINT_FULL, hintstyle);
  }

  Preferences::SetInt("zoom.stealth.text.freetype_antialias", 0);
  {
    ScopedFcPattern p;
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetBool(p.mPattern, FC_ANTIALIAS, 0, &antialias));
    EXPECT_EQ(FcFalse, antialias);
  }
  Preferences::SetInt("zoom.stealth.text.freetype_antialias", 1);
  {
    ScopedFcPattern p;
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetBool(p.mPattern, FC_ANTIALIAS, 0, &antialias));
    EXPECT_EQ(FcTrue, antialias);
  }
  Preferences::SetInt("zoom.stealth.text.freetype_antialias", -1);
  {
    ScopedFcPattern p;
    FcPatternAddBool(p.mPattern, FC_ANTIALIAS, FcFalse);
    StealthDeclareTextOptions(p.mPattern);
    ASSERT_EQ(FcResultMatch,
              FcPatternGetBool(p.mPattern, FC_ANTIALIAS, 0, &antialias));
    EXPECT_EQ(FcFalse, antialias);
  }
}

TEST(StealthFcFontList, TableFaceIndex)
{
  EXPECT_EQ(0u, StealthTableFaceIndex(nullptr));
}

TEST(StealthFcFontList, PreferFileUnscaledFont)
{
  using mozilla::gfx::UnscaledFontFontconfig;
  {
    // Mozilla already built it on the path: kept (and aFile, moved from, is
    // not read).
    RefPtr<UnscaledFontFontconfig> font =
        new UnscaledFontFontconfig(std::string("/x/a.ttf"), 1, nullptr);
    UnscaledFontFontconfig* before = font;
    std::string file = "/x/other.ttf";
    StealthPreferFileUnscaledFont(nullptr, font, file, 2, nullptr);
    EXPECT_EQ(before, font.get());
  }
  {
    // A page's font: no path, no file. Kept.
    RefPtr<UnscaledFontFontconfig> font =
        new UnscaledFontFontconfig(RefPtr<mozilla::gfx::SharedFTFace>());
    UnscaledFontFontconfig* before = font;
    std::string file;
    StealthPreferFileUnscaledFont(nullptr, font, file, 0, nullptr);
    EXPECT_EQ(before, font.get());
  }
  {
    // A bundled face: no path but the file is known. Replaced by one on the
    // path, which goes into the entry's cache.
    ScopedFcPattern p;
    RefPtr<gfxFontconfigFontEntry> entry =
        new gfxFontconfigFontEntry("bundled"_ns, p.mPattern);
    RefPtr<UnscaledFontFontconfig> font =
        new UnscaledFontFontconfig(RefPtr<mozilla::gfx::SharedFTFace>());
    std::string file = "/x/d.ttc";
    StealthPreferFileUnscaledFont(entry, font, file, 4, nullptr);
    ASSERT_TRUE(font);
    EXPECT_EQ("/x/d.ttc", font->GetFile());
    EXPECT_EQ(4u, font->GetIndex());
    // Moved from in this case only.
    EXPECT_TRUE(file.empty());
  }
}
