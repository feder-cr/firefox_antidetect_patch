/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A recorded font descriptor in the parent (Linux): with the empty fontconfig
// configuration installed, a font of the bundle gives an UnscaledFont (it used
// to give null, because FcFontMatch matches nothing in an empty configuration,
// and page.screenshot() with text never answered); a path outside the bundle,
// one that climbs out with "..", and a symlink that leaves it give null.

#include "gtest/gtest.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <string>

#include "StealthFcEmptyConfig.h"
#include "mozilla/RefPtr.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/StealthFontDescriptorPath.h"

using namespace mozilla::gfx;

namespace {

struct Tree {
  std::string root;
  std::string bundle;
  std::string outside;

  Tree() {
    char tmpl[] = "/tmp/stealth-fonts-XXXXXX";
    root = mkdtemp(tmpl);
    bundle = root + "/fonts";
    mkdir(bundle.c_str(), 0700);
    outside = root + "/outside.ttf";
    Touch(bundle + "/arial.ttf");
    Touch(outside);
    symlink(outside.c_str(), (bundle + "/link.ttf").c_str());
  }
  ~Tree() {
    unlink((bundle + "/link.ttf").c_str());
    unlink((bundle + "/arial.ttf").c_str());
    unlink(outside.c_str());
    rmdir(bundle.c_str());
    rmdir(root.c_str());
  }
  static void Touch(const std::string& aPath) { std::ofstream(aPath) << "x"; }
};

bool Accepts(const Tree& aTree, const std::string& aPath) {
  RefPtr<UnscaledFont> font = StealthUnscaledFontFromBundleDir(
      aTree.bundle, reinterpret_cast<const uint8_t*>(aPath.data()),
      aPath.size(), 0);
  return !!font;
}

}  // namespace

TEST(StealthFontDescriptorPath, BundleFontWithEmptyFontconfig)
{
  ASSERT_TRUE(StealthFcEmptyConfig());
  Tree tree;
  EXPECT_TRUE(Accepts(tree, tree.bundle + "/arial.ttf"));
  // A path that goes through the bundle and comes back is still inside.
  EXPECT_TRUE(Accepts(tree, tree.bundle + "/../fonts/arial.ttf"));
}

TEST(StealthFontDescriptorPath, UntrustedIndex)
{
  Tree tree;
  std::string path = tree.bundle + "/arial.ttf";
  auto make = [&](uint32_t aIndex) {
    return StealthUnscaledFontFromBundleDir(
        tree.bundle, reinterpret_cast<const uint8_t*>(path.data()),
        path.size(), aIndex);
  };
  // Bit 31 (negative for FreeType) is refused at once.
  RefPtr<UnscaledFont> negative = make(0x80000000u);
  EXPECT_FALSE(negative);
  RefPtr<UnscaledFont> allBits = make(0xffffffffu);
  EXPECT_FALSE(allBits);
  // Out-of-range face, non-existent instance, or both: no crash. The face is
  // opened later by FreeType (UnscaledFontFreeType::InitFace), which fails for
  // an index that does not exist, and CreateScaledFont answers nullptr; here
  // it is asked with no instance data, which it also refuses.
  for (uint32_t index : {0u, 7u, 0xffffu, 0x10000u, 0x10005u, 0x7fffffffu}) {
    RefPtr<UnscaledFont> font = make(index);
    if (font) {
      RefPtr<ScaledFont> scaled =
          font->CreateScaledFont(12.0f, nullptr, 0, nullptr, 0);
      EXPECT_FALSE(scaled);
    }
  }
}

TEST(StealthFontDescriptorPath, HandlesDescriptors)
{
  EXPECT_TRUE(StealthHandlesFontDescriptors());
}

TEST(StealthFontDescriptorPath, RefusesOutsideTheBundle)
{
  Tree tree;
  EXPECT_FALSE(Accepts(tree, tree.outside));
  EXPECT_FALSE(Accepts(tree, "/etc/passwd"));
  EXPECT_FALSE(Accepts(tree, tree.bundle));  // the directory is not a font
  EXPECT_FALSE(Accepts(tree, tree.bundle + "/missing.ttf"));
  EXPECT_FALSE(Accepts(tree, ""));
}

TEST(StealthFontDescriptorPath, RefusesDotDotThatLeaves)
{
  Tree tree;
  EXPECT_FALSE(Accepts(tree, tree.bundle + "/../outside.ttf"));
  EXPECT_FALSE(Accepts(tree, tree.bundle + "/../../etc/passwd"));
}

TEST(StealthFontDescriptorPath, RefusesSymlinkThatLeaves)
{
  Tree tree;
  EXPECT_FALSE(Accepts(tree, tree.bundle + "/link.ttf"));
}

TEST(StealthFontDescriptorPath, RefusesSiblingWithTheSamePrefix)
{
  Tree tree;
  std::string sibling = tree.root + "/fonts-evil";
  mkdir(sibling.c_str(), 0700);
  Tree::Touch(sibling + "/a.ttf");
  EXPECT_FALSE(Accepts(tree, sibling + "/a.ttf"));
  unlink((sibling + "/a.ttf").c_str());
  rmdir(sibling.c_str());
}
