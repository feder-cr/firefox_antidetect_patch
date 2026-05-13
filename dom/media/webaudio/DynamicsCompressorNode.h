/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DynamicsCompressorNode_h_
#define DynamicsCompressorNode_h_

#include "AudioNode.h"
#include "AudioParam.h"
#include "mozilla/StaticPrefs_zoom.h"

namespace mozilla::dom {

class AudioContext;
struct DynamicsCompressorOptions;

class DynamicsCompressorNode final : public AudioNode {
 public:
  static already_AddRefed<DynamicsCompressorNode> Create(
      AudioContext& aAudioContext, const DynamicsCompressorOptions& aOptions,
      ErrorResult& aRv);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(DynamicsCompressorNode, AudioNode)

  static already_AddRefed<DynamicsCompressorNode> Constructor(
      const GlobalObject& aGlobal, AudioContext& aAudioContext,
      const DynamicsCompressorOptions& aOptions, ErrorResult& aRv) {
    return Create(aAudioContext, aOptions, aRv);
  }

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  AudioParam* Threshold() const { return mThreshold; }

  AudioParam* Knee() const { return mKnee; }

  AudioParam* Ratio() const { return mRatio; }

  AudioParam* Attack() const { return mAttack; }

  // Called GetRelease to prevent clashing with the nsISupports::Release name
  AudioParam* GetRelease() const { return mRelease; }

  float Reduction() const { return mReduction; }

  const char* NodeType() const override { return "DynamicsCompressorNode"; }

  size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const override;
  size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const override;

  void SetReduction(float aReduction) {
    MOZ_ASSERT(NS_IsMainThread());
    // Stealth: add per-session noise to compressorGainReduction so it varies.
    int32_t seed = mozilla::StaticPrefs::zoom_stealth_fpp_hw_seed();
    if (seed > 0) {
      uint32_t h = uint32_t(seed) ^ 0xA5A5A5A5u;
      h ^= (h >> 16); h *= 0x45d9f3bu; h ^= (h >> 16);
      aReduction += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000))
                    * 1.0e-5f;
    }
    mReduction = aReduction;
  }

  void SetChannelCountModeValue(ChannelCountMode aMode,
                                ErrorResult& aRv) override {
    if (aMode == ChannelCountMode::Max) {
      aRv.ThrowNotSupportedError("Cannot set channel count mode to \"max\"");
      return;
    }
    AudioNode::SetChannelCountModeValue(aMode, aRv);
  }

  void SetChannelCount(uint32_t aChannelCount, ErrorResult& aRv) override {
    if (aChannelCount > 2) {
      aRv.ThrowNotSupportedError(
          nsPrintfCString("%u is greater than 2", aChannelCount));
      return;
    }
    AudioNode::SetChannelCount(aChannelCount, aRv);
  }

 private:
  explicit DynamicsCompressorNode(AudioContext* aContext);
  ~DynamicsCompressorNode() = default;

  RefPtr<AudioParam> mThreshold;
  RefPtr<AudioParam> mKnee;
  RefPtr<AudioParam> mRatio;
  float mReduction;
  RefPtr<AudioParam> mAttack;
  RefPtr<AudioParam> mRelease;
};

}  // namespace mozilla::dom

#endif
