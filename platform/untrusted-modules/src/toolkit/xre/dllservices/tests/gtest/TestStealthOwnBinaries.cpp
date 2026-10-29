/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Which modules the untrusted-modules processor treats as this build's own,
// and so skips the signature lookup for (ModuleRecord::IsInDirectoryOf): it
// hashed every unsigned DLL of the install for a catalog signature that
// cannot exist, 1.26 s of CPU per launch.

#include "gtest/gtest.h"

#include "mozilla/UntrustedModulesData.h"
#include "nsString.h"

using mozilla::ModuleRecord;

TEST(StealthOwnBinaries, BesideTheExeAndBelowIt)
{
  const nsString exe(u"C:\\Program Files\\App\\firefox.exe"_ns);
  EXPECT_TRUE(ModuleRecord::IsInDirectoryOf(u"C:\\Program Files\\App\\xul.dll"_ns, exe));
  EXPECT_TRUE(ModuleRecord::IsInDirectoryOf(
      u"C:\\Program Files\\App\\gmp-clearkey\\0.1\\clearkey.dll"_ns, exe));
}

TEST(StealthOwnBinaries, CaseInsensitiveAsWindowsPathsAre)
{
  EXPECT_TRUE(ModuleRecord::IsInDirectoryOf(
      u"c:\\program files\\app\\XUL.DLL"_ns, u"C:\\Program Files\\App\\firefox.exe"_ns));
}

TEST(StealthOwnBinaries, ElsewhereKeepsTheLookup)
{
  const nsString exe(u"C:\\Program Files\\App\\firefox.exe"_ns);
  EXPECT_FALSE(ModuleRecord::IsInDirectoryOf(u"C:\\Windows\\System32\\ntdll.dll"_ns, exe));
  // A sibling directory whose name starts like ours is not ours.
  EXPECT_FALSE(ModuleRecord::IsInDirectoryOf(u"C:\\Program Files\\App2\\evil.dll"_ns, exe));
  // No directory in the exe path: never treated as ours.
  EXPECT_FALSE(ModuleRecord::IsInDirectoryOf(u"xul.dll"_ns, u"firefox.exe"_ns));
}
