/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Under bundle-only, a content process does not build the platform's
// collection of the bundled font files: it never reads it, and building it
// opened all 121 files twice in every content process at every launch
// (StealthBundleFontList::NeedsPlatformBundledCollection).
//
// It also holds the tests of the other bundle-only font-list functions that
// need no bundle on disk: the host substitute tables, the declared generics,
// the Windows fallback names and the face selection inside a .ttc.

#include "gtest/gtest.h"

#include <cstring>

#include "StealthBundleFontList.h"
#include "gfxFontUtils.h"
#include "mozilla/Preferences.h"
#include "nsString.h"
#include "nsTArray.h"

TEST(StealthBundledCollection, ContentProcessUnderBundleOnlyBuildsNone)
{
  EXPECT_FALSE(StealthBundleFontList::NeedsPlatformBundledCollection(
      /* aBundleOnly */ true, /* aIsParentProcess */ false));
}

TEST(StealthBundledCollection, ParentKeepsItForTheFontInfoLoader)
{
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(true, true));
}

TEST(StealthBundledCollection, WithoutBundleOnlyEveryProcessBuildsItAsUpstream)
{
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(false, false));
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(false, true));
}

// The hook in CreateBundledFontsCollection skips the collection only while the
// shared list is built; the in-process fallback list builds it as before.
TEST(StealthBundledCollection, SkippedOnlyForTheSharedListOfAContentProcess)
{
  EXPECT_TRUE(StealthBundleFontList::SkipsPlatformBundledCollection(
      /* aBuildingSharedList */ true, /* aBundleOnly */ true,
      /* aIsParentProcess */ false));
  EXPECT_FALSE(StealthBundleFontList::SkipsPlatformBundledCollection(
      false, true, false));
  EXPECT_FALSE(
      StealthBundleFontList::SkipsPlatformBundledCollection(true, true, true));
  EXPECT_FALSE(
      StealthBundleFontList::SkipsPlatformBundledCollection(true, false, false));
}

TEST(StealthBundledCollection, HostSubstitutesApplyAsUpstreamWithoutBundleOnly)
{
  EXPECT_TRUE(StealthBundleFontList::UsesHostSubstitutes(false));
}

namespace {

void StealthSetGenerics(const char* aSpec) {
  mozilla::Preferences::SetCString("zoom.stealth.fonts.generics", aSpec);
}

}  // namespace

TEST(StealthFontList, ExactLangGroupBeatsDefaultWhateverTheOrder)
{
  for (const char* spec : {"serif||Times New Roman\nserif|ja|MS Mincho",
                           "serif|ja|MS Mincho\nserif||Times New Roman"}) {
    StealthSetGenerics(spec);
    nsAutoCString out;
    EXPECT_TRUE(StealthDeclaredGenericFont(mozilla::StyleGenericFontFamily::Serif,
                                           "ja", out));
    EXPECT_TRUE(out.EqualsLiteral("MS Mincho"));
    EXPECT_TRUE(StealthDeclaredGenericFont(mozilla::StyleGenericFontFamily::Serif,
                                           "x-western", out));
    EXPECT_TRUE(out.EqualsLiteral("Times New Roman"));
  }
  mozilla::Preferences::ClearUser("zoom.stealth.fonts.generics");
}

TEST(StealthFontList, EmptyDeclarationAndNonForcedGenericsDeclareNothing)
{
  nsAutoCString out;
  StealthSetGenerics("");
  EXPECT_FALSE(StealthDeclaredGenericFont(mozilla::StyleGenericFontFamily::Serif,
                                          "ja", out));
  StealthSetGenerics("serif||Times New Roman");
  EXPECT_FALSE(StealthDeclaredGenericFont(
      mozilla::StyleGenericFontFamily::Fantasy, "", out));
  EXPECT_FALSE(StealthDeclaredGenericFont(
      mozilla::StyleGenericFontFamily::MozEmoji, "", out));
  mozilla::Preferences::ClearUser("zoom.stealth.fonts.generics");
}

TEST(StealthFontList, PrependGoesToIndexZeroOnlyUnderBundleOnly)
{
  StealthSetGenerics("monospace||Consolas");
  AutoTArray<nsCString, 4> fams;
  fams.AppendElement("Courier New"_ns);
  StealthPrependDeclaredGeneric(
      false, mozilla::StyleGenericFontFamily::Monospace, "", fams);
  StealthPrependEmojiFamily(false, fams);
  ASSERT_EQ(fams.Length(), 1u);
  StealthPrependDeclaredGeneric(
      true, mozilla::StyleGenericFontFamily::Monospace, "", fams);
  StealthPrependEmojiFamily(true, fams);
  ASSERT_EQ(fams.Length(), 3u);
  EXPECT_TRUE(fams[0].EqualsLiteral("Segoe UI Emoji"));
  EXPECT_TRUE(fams[1].EqualsLiteral("Consolas"));
  EXPECT_TRUE(fams[2].EqualsLiteral("Courier New"));
  mozilla::Preferences::ClearUser("zoom.stealth.fonts.generics");
}

TEST(StealthFontList, SystemUiIsSegoeUi)
{
  nsTArray<nsCString> fams;
  StealthSystemUIFamilies(fams);
  ASSERT_EQ(fams.Length(), 1u);
  EXPECT_TRUE(fams[0].EqualsLiteral("Segoe UI"));
}

TEST(StealthFontList, AliasIsIdentityWhenNotBundleOnly)
{
  nsAutoCString key("helv");
  StealthApplyAlias(false, key);  // must not touch the bundle
  EXPECT_TRUE(key.EqualsLiteral("helv"));
}

TEST(StealthFallbackNames, ColorPreferredLeadsWithSegoeUIEmoji)
{
  nsTArray<const char*> list;
  StealthAppendWindowsFallbackNames(/* aPrefersColor */ true, list);
  ASSERT_EQ(list.Length(), 5u);
  EXPECT_STREQ(list[0], "Segoe UI Emoji");
  EXPECT_STREQ(list[1], "Arial");
  EXPECT_STREQ(list[2], "Segoe UI");
  EXPECT_STREQ(list[3], "Segoe UI Symbol");
  EXPECT_STREQ(list[4], "Cambria Math");
}

TEST(StealthFallbackNames, TextPresentationOmitsEmojiFace)
{
  nsTArray<const char*> list;
  StealthAppendWindowsFallbackNames(/* aPrefersColor */ false, list);
  ASSERT_EQ(list.Length(), 4u);
  EXPECT_STREQ(list[0], "Arial");
  EXPECT_STREQ(list[1], "Segoe UI");
  EXPECT_STREQ(list[2], "Segoe UI Symbol");
  EXPECT_STREQ(list[3], "Cambria Math");
}

TEST(StealthFallbackNames, AppendsAfterWhatIsAlreadyInTheList)
{
  nsTArray<const char*> list;
  list.AppendElement("Keep");
  StealthAppendWindowsFallbackNames(false, list);
  ASSERT_EQ(list.Length(), 5u);
  EXPECT_STREQ(list[0], "Keep");
}

// Face selection inside a .ttc collection (gfxFontUtils::FindSfntOffset,
// FindTableDirEntry, GetTableFromFontData with a face index): the table of the
// requested face is returned, an out-of-range face returns nothing, and a
// single-face font ignores the index.

namespace {

void TtcPutBE16(uint8_t* aDst, uint16_t aValue) {
  aDst[0] = uint8_t(aValue >> 8);
  aDst[1] = uint8_t(aValue);
}

void TtcPutBE32(uint8_t* aDst, uint32_t aValue) {
  TtcPutBE16(aDst, uint16_t(aValue >> 16));
  TtcPutBE16(aDst + 2, uint16_t(aValue));
}

// One sfnt header (12 bytes) followed by one 'name' table directory entry
// (16 bytes) at aOffset; the table itself lives at aTableOffset (4 bytes).
void TtcWriteFace(uint8_t* aBuf, uint32_t aOffset, uint32_t aTableOffset) {
  uint8_t* p = aBuf + aOffset;
  TtcPutBE32(p, 0x00010000);
  TtcPutBE16(p + 4, 1);   // numTables
  TtcPutBE16(p + 6, 16);  // searchRange
  TtcPutBE16(p + 8, 0);   // entrySelector
  TtcPutBE16(p + 10, 0);  // rangeShift
  TtcPutBE32(p + 12, TRUETYPE_TAG('n', 'a', 'm', 'e'));
  TtcPutBE32(p + 16, 0);  // checkSum
  TtcPutBE32(p + 20, aTableOffset);
  TtcPutBE32(p + 24, 4);  // length
}

constexpr uint32_t kTtcFace0 = 20;
constexpr uint32_t kTtcFace1 = 48;
constexpr uint32_t kTtcTable0 = 76;
constexpr uint32_t kTtcTable1 = 80;

// 'ttcf', version 1.0, two faces; table offsets are relative to the file start.
void TtcBuild(uint8_t (&aBuf)[84]) {
  memset(aBuf, 0, sizeof(aBuf));
  memcpy(aBuf, "ttcf", 4);
  TtcPutBE16(aBuf + 4, 1);
  TtcPutBE16(aBuf + 6, 0);
  TtcPutBE32(aBuf + 8, 2);  // numFonts
  TtcPutBE32(aBuf + 12, kTtcFace0);
  TtcPutBE32(aBuf + 16, kTtcFace1);
  TtcWriteFace(aBuf, kTtcFace0, kTtcTable0);
  TtcWriteFace(aBuf, kTtcFace1, kTtcTable1);
  memcpy(aBuf + kTtcTable0, "AAAA", 4);
  memcpy(aBuf + kTtcTable1, "BBBB", 4);
}

}  // namespace

TEST(StealthTtcFace, FindSfntOffsetPicksTheRequestedFace)
{
  uint8_t buf[84];
  TtcBuild(buf);
  EXPECT_EQ(reinterpret_cast<const void*>(gfxFontUtils::FindSfntOffset(buf, 0)),
            static_cast<const void*>(buf + kTtcFace0));
  EXPECT_EQ(reinterpret_cast<const void*>(gfxFontUtils::FindSfntOffset(buf, 1)),
            static_cast<const void*>(buf + kTtcFace1));
}

TEST(StealthTtcFace, FindSfntOffsetOutOfRangeIsNull)
{
  uint8_t buf[84];
  TtcBuild(buf);
  EXPECT_EQ(gfxFontUtils::FindSfntOffset(buf, 2), nullptr);
  EXPECT_EQ(gfxFontUtils::FindSfntOffset(buf, 0xFFFFFFFFu), nullptr);
}

TEST(StealthTtcFace, TableComesFromTheRequestedFace)
{
  uint8_t buf[84];
  TtcBuild(buf);
  const uint32_t tag = TRUETYPE_TAG('n', 'a', 'm', 'e');
  for (uint32_t face = 0; face < 2; ++face) {
    hb_blob_t* blob = gfxFontUtils::GetTableFromFontData(buf, tag, face);
    ASSERT_NE(blob, nullptr);
    unsigned int len = 0;
    const char* data = hb_blob_get_data(blob, &len);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(0, memcmp(data, face == 0 ? "AAAA" : "BBBB", 4));
    EXPECT_EQ(static_cast<const void*>(data),
              static_cast<const void*>(buf + (face == 0 ? kTtcTable0
                                                         : kTtcTable1)));
    hb_blob_destroy(blob);
  }
  EXPECT_EQ(gfxFontUtils::GetTableFromFontData(buf, tag, 2), nullptr);
  EXPECT_EQ(gfxFontUtils::FindTableDirEntry(buf, tag, 2), nullptr);
}

TEST(StealthTtcFace, SingleFaceFontIgnoresTheIndex)
{
  uint8_t buf[32];
  memset(buf, 0, sizeof(buf));
  TtcWriteFace(buf, 0, 28);
  memcpy(buf + 28, "CCCC", 4);
  const uint32_t tag = TRUETYPE_TAG('n', 'a', 'm', 'e');
  EXPECT_EQ(reinterpret_cast<const void*>(gfxFontUtils::FindSfntOffset(buf, 7)),
            static_cast<const void*>(buf));
  for (uint32_t face : {0u, 7u}) {
    hb_blob_t* blob = gfxFontUtils::GetTableFromFontData(buf, tag, face);
    ASSERT_NE(blob, nullptr);
    unsigned int len = 0;
    EXPECT_EQ(0, memcmp(hb_blob_get_data(blob, &len), "CCCC", 4));
    hb_blob_destroy(blob);
  }
  // The two-argument form still works (default face 0).
  EXPECT_NE(gfxFontUtils::FindTableDirEntry(buf, tag), nullptr);
}

TEST(StealthBundledFamilyFilter, HostFamilyIsDroppedUnderBundleOnly)
{
  nsTHashSet<nsCStringHashKey> bundled;
  bundled.Insert("Segoe UI"_ns);
  EXPECT_FALSE(StealthDropFamily(true, bundled, "Segoe UI"_ns));
  EXPECT_TRUE(StealthDropFamily(true, bundled, "Helvetica Neue"_ns));
}

TEST(StealthBundledFamilyFilter, NothingIsDroppedWithoutBundleOnly)
{
  nsTHashSet<nsCStringHashKey> bundled;
  EXPECT_FALSE(StealthDropFamily(false, bundled, "Helvetica Neue"_ns));
}

TEST(StealthBundledFamilyFilter, DefaultUiFamilyIsTheWindowsPersona)
{
  EXPECT_TRUE(StealthUiFamily().EqualsLiteral("Segoe UI"));
}
