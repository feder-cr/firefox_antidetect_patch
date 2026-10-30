/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The declared decode support (dom/media/StealthMediaDeclaration.*,
// zoom.stealth.media.decode_support): what a page learns about decoding comes
// from the declaration, not from the machine. PDMFactory has two query roads,
// Supports (sync) and SupportsAsync, and both must answer from it. Since 159
// the WebRTC branch of mediaCapabilities.decodingInfo asks SupportsAsync, which
// did not consult the declaration and leaked the host's real hardware-decode
// bit.
//
// The mime types below are made up on purpose: no platform module decodes
// them, so without the declaration every host answers the empty set, and a
// declared set can never be mistaken for what the host really reports.

#include "MediaInfo.h"
#include "PDMFactory.h"
#include "PlatformDecoderModule.h"
#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/gtest/WaitFor.h"

using namespace mozilla;
using media::DecodeSupport;
using media::DecodeSupportSet;

namespace {

constexpr char kDeclarationPref[] = "zoom.stealth.media.decode_support";

// Newline separated "mime|flags" entries, see the pref's comment.
constexpr char kDeclaration[] =
    "video/x-stealth-sw|sw\n"
    "video/x-stealth-hw|hw\n"
    "video/x-stealth-swhw|swhw\n"
    "video/x-stealth-no|no";

// Sets the pref, puts it back at the end of the test.
class DecodeSupportPref {
 public:
  explicit DecodeSupportPref(const char* aSpec) {
    Preferences::GetCString(kDeclarationPref, mOld);
    Preferences::SetCString(kDeclarationPref, nsDependentCString(aSpec));
  }
  ~DecodeSupportPref() { Preferences::SetCString(kDeclarationPref, mOld); }

 private:
  nsAutoCString mOld;
};

DecodeSupportSet SyncSupport(const char* aMime) {
  VideoInfo info;
  info.mMimeType = nsDependentCString(aMime);
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  return factory->Supports(SupportDecoderParams(info), nullptr);
}

// Off the main thread, as SupportsAsync requires.
DecodeSupportSet AsyncSupport(const char* aMime) {
  VideoInfo info;
  info.mMimeType = nsDependentCString(aMime);
  RefPtr<TaskQueue> queue = TaskQueue::Create(
      GetMediaThreadPool(MediaThreadType::SUPERVISOR), "TestStealthDecodeAsync");
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  DecodeSupportSet result = WaitForResolve(
      InvokeAsync(queue, __func__, [&info, factory]() {
        return factory->SupportsAsync(SupportDecoderParams(info));
      }));
  queue->BeginShutdown();
  queue->AwaitShutdownAndIdle();
  return result;
}

}  // namespace

TEST(StealthMediaDeclaration, SupportsAsyncAnswersFromTheDeclaration)
{
  DecodeSupportPref pref(kDeclaration);
  EXPECT_EQ(AsyncSupport("video/x-stealth-sw"),
            DecodeSupportSet{DecodeSupport::SoftwareDecode});
  EXPECT_EQ(AsyncSupport("video/x-stealth-hw"),
            DecodeSupportSet{DecodeSupport::HardwareDecode});
  EXPECT_EQ(AsyncSupport("video/x-stealth-swhw"),
            DecodeSupportSet(DecodeSupport::SoftwareDecode,
                             DecodeSupport::HardwareDecode));
  EXPECT_EQ(AsyncSupport("video/x-stealth-no"), DecodeSupportSet{});
}

// The two roads must never diverge: a page can reach either one.
TEST(StealthMediaDeclaration, SupportsAndSupportsAsyncAgree)
{
  DecodeSupportPref pref(kDeclaration);
  for (const char* mime :
       {"video/x-stealth-sw", "video/x-stealth-hw", "video/x-stealth-swhw",
        "video/x-stealth-no", "video/x-stealth-undeclared"}) {
    EXPECT_EQ(SyncSupport(mime), AsyncSupport(mime)) << mime;
  }
}

// An undeclared type falls through to the real answer, on both roads.
TEST(StealthMediaDeclaration, UndeclaredTypeFallsThrough)
{
  DecodeSupportPref pref(kDeclaration);
  EXPECT_EQ(AsyncSupport("video/x-stealth-undeclared"), DecodeSupportSet{});
}
