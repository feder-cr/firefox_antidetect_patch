/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_STEALTHMEDIADECLARATION_H_
#define DOM_MEDIA_STEALTHMEDIADECLARATION_H_

#include "MediaCodecsSupport.h"
#include "mozilla/Maybe.h"
#include "nsStringFwd.h"

namespace mozilla {

/**
 * What this build reports it can decode, DECLARED by invisible_core instead of
 * derived from what the machine happens to have installed.
 *
 * WHERE THE HOST-DEPENDENCE ACTUALLY IS, measured 2026-08-08 across four arms
 * (retail Windows headed, retail Windows headless, our Windows, our Linux) over
 * 45 media types and 8 decodingInfo configurations:
 *
 *   our Windows vs retail Windows      45/45 and 8/8 identical
 *   our Linux   vs retail Windows      12 canPlayType rows differ, all of them
 *                                      avc1 with a supported profile, plus
 *                                      hev1/hvc1
 *
 * So exactly one question is host-dependent: does this build have an H.264 (or
 * HEVC) decoder. The Linux build ships ffvpx with H.264 compiled out and reaches
 * for the system libavcodec, so two Linux users differ from EACH OTHER, which is
 * worse than differing from Windows.
 *
 * WHAT IS NOT HOST-DEPENDENT, and this correction matters because the previous
 * version of this table was built on the opposite belief. The rule that
 * baseline/main/extended/high answer and high10/high422/high444 do not, and that
 * the level must fall in [1, 6.2], is IsAllowedH264Codec in VideoUtils.cpp:1109
 * - a compiled Gecko constant, applied identically on every platform. Measuring
 * it on retail Windows and reading it as a Windows fact produced a 112-vs-84
 * split that looked like a rule worth declaring and was simply Gecko's own
 * arithmetic. Verified at the edges: avc1.644000 (level 0) and avc1.64403F
 * (level 6.3) answer empty on retail, exactly as that function says they must.
 *
 * WHY THE DECLARATION MOVED DOWN A LAYER. It used to sit in
 * DecoderTraits::CanHandleMediaType as a table of whole type STRINGS, which
 * could only ever hold the strings somebody had seen: measured that day, it
 * carried 3 of the 12 rows that actually differed. The space of type strings is
 * not enumerable - profile x constraint x level x container x the audio codec
 * travelling with it - so no list of strings can be complete.
 *
 * One layer down it is finite and small. PDMFactory::Supports is asked about a
 * TRACK mime type: video/avc, video/hevc, video/av1, video/vp9, audio/opus, a
 * dozen or so names Gecko itself defines. Declaring those covers every string
 * that can ever be built out of them, and everything above - container rules,
 * the codec-list AND, the profile and level windows - stays Gecko's own
 * host-independent logic. That is the difference between declaring a result and
 * declaring the alphabet the results are made of.
 *
 * PDMFactory::Supports is a QUERY path only; decoder creation does not go
 * through it. CreateDecoder and GetDecoderModule ask each platform module's own
 * Supports, so creating a decoder is untouched. Verified by reading the callers
 * against the 157.0.1 tree:
 *
 *   - Through PDMFactorySupport::IsSupported -> PDMFactorySupport::Supports ->
 *     PDMFactory::Supports: MP4Decoder::IsSupportedType, MatroskaDecoder,
 *     WebMDecoder, and MediaCapabilities when the codec-support cache pref is
 *     off (the synchronous branch).
 *   - Through the instance PDMFactory::SupportsMimeType, which builds a track
 *     info and calls Supports: KeySystemConfig, MediaKeySystemAccess, GMPParent.
 *   - NOT through MFMediaEngineDecoderModule::SupportsConfig: it asks a
 *     RemoteDecoderModule, not a PDMFactory.
 *   - NOT through PDMFactory::SupportsAsync, which asks each platform module
 *     directly. So the cache-backed MediaCapabilities path (cache pref on)
 *     does not see this declaration; the synchronous path does.
 *
 * The declaration is consulted BEFORE the EME module, so on a build where EME is
 * present the declared answer wins over the EME answer.
 *
 * THE COST, stated because it is a trade and not a win: a declared "yes" for a
 * codec this build cannot decode means a site picks H.264 and the video does not
 * play, where today it falls back to WebM and plays. Fidelity to what a Windows
 * Firefox reports is bought with broken playback on Linux. The way out is to
 * ship the decoder, not to change this answer back.
 */

/**
 * Declared decode support for a track mime type.
 *
 * Returns false when invisible_core has not declared this type, and the caller
 * must then fall through to the real platform answer - an override, never a
 * replacement, so the failure mode stays "we fail to disguise" rather than "the
 * page breaks".
 *
 * On true, aSoftware/aHardware carry the declared DecodeSupport flags; both
 * false means the declaration is "this build cannot decode it".
 */
bool StealthDeclaredDecodeSupport(const nsACString& aTrackMimeType,
                                  bool& aSoftware, bool& aHardware);

/**
 * The declaration as a DecodeSupportSet, ready for PDMFactory::Supports to
 * return: Nothing() when StealthDeclaredDecodeSupport has not declared this
 * type (the caller then falls through to the real platform answer), otherwise
 * Some(set) with SoftwareDecode and/or HardwareDecode added as declared. Some
 * with an empty set is the declaration "this build cannot decode it" and must
 * be returned as it is, not treated as a fall-through.
 *
 * Called first in PDMFactory::Supports: what this build reports it can decode is
 * DECLARED, not derived from what the machine happens to have installed. That
 * is the layer where the host-dependence lives and the key space is finite -
 * see the measurement above for why it sits here rather than in DecoderTraits,
 * where it was a list of type strings that could never be complete. Everything
 * above (container rules, the codec-list AND, the H.264 profile and level
 * windows) is Gecko's own logic and already identical on every platform.
 */
Maybe<media::DecodeSupportSet> StealthDeclaredDecodeSupportSet(
    const nsACString& aTrackMimeType);

/**
 * Declared mediaCapabilities.decodingInfo answer, keyed by container mime type
 * plus the codec token's family prefix (the part before the first '.', so
 * "avc1", "hev1", "av01", "vp09", "vp8").
 *
 * WHY NOT THE TRACK MIME. powerEfficient is not a property of it. Measured on
 * retail Windows, headed: vp09.00.10.08 reports powerEfficient TRUE and a bare
 * vp9 reports FALSE - in the SAME container, video/webm - and both build a track
 * whose mime type is video/vp9. The fully specified string carries the profile
 * and bit depth the hardware check needs; the bare one does not. Keying on the
 * track mime would have to pick one of the two and would then be wrong for the
 * other, in the direction that MOVES a row our two builds already agree on.
 *
 * The container is part of the key as well, which the measurement does not
 * require - av01 and vp09 answer the same in mp4 and in webm - and it is kept
 * because it is the identity of what was actually measured. Four extra rows.
 *
 * WHY THE CALLER GATES ON Gecko's OWN ANSWER FIRST. The family key is too coarse
 * in exactly one place: avc1.424028 and avc1.6E4028 share the family "avc1" and
 * retail answers for the first and not the second. Splitting the key finer would
 * mean declaring the H.264 profile space, which the paragraph above establishes
 * is Gecko's compiled constant and not ours. So StealthDeclaredDecodingInfoFor
 * asks CanHandleContainerType first and only consults this table for a type Gecko
 * already considers playable - a precondition on host-independent logic, not a
 * fallback to host-dependent logic.
 *
 * WHY IT IS MEASURED HEADED. The judge is the retail Firefox a user runs, and
 * powerEfficient answers differently under -headless: avc1, av01 and vp09 all
 * report FALSE headless and TRUE headed on the same machine, because the
 * hardware check has no compositor to ask. Measuring the judge in the mode our
 * product runs in rather than the mode our users' machines run in would have
 * pinned the wrong three values.
 */
bool StealthDeclaredDecodingInfo(const nsACString& aContainerMimeType,
                                 const nsACString& aCodec, bool& aSupported,
                                 bool& aSmooth, bool& aPowerEfficient);

namespace dom {

struct MediaCapabilitiesDecodingInfo;
struct MediaDecodingConfiguration;

/**
 * The declared mediaCapabilities.decodingInfo answer for a whole video
 * configuration, or Nothing() when it is not declared (the caller then keeps
 * the engine's own answer). Called from
 * MediaCapabilities::CreateNonWebRTCDecodingInfo on both resolve branches.
 *
 * Every codec named in the video configuration has to be declared, and the
 * three answers are ANDed across them - the same shape the engine uses in
 * MediaCapabilities::CreateNonWebRTCDecodingInfo, and the same shape canPlayType
 * uses for a codec list. One undeclared codec means the whole configuration is
 * undeclared and falls through to the real answer: an override, never a
 * replacement.
 *
 * Audio is deliberately not consulted. Gecko answers true/true/true for every
 * audio track it can decode (the kAudioTrack check in
 * MediaCapabilities::CreateNonWebRTCDecodingInfo, which is upstream code), and
 * audio decoding is bundled, so it carries no host dependence to declare away.
 *
 * THE HOOK APPLIES ON BOTH BRANCHES, the reject and the resolve one.
 * decodingInfo asks two questions - "is this type supported" and "can the
 * decoder actually handle this stream" - and the second one queries a real
 * decoder, so it is host-dependent twice over. It REJECTS on a build with no
 * H.264, and its powerEfficient comes from whether a hardware decoder answered.
 *
 * The previous version of this hook ran ONLY on the reject branch and answered
 * true/true/true, and it was wrong in both directions. It never reached Windows
 * at all, where the engine resolves and computes the three fields itself. And
 * on Linux it did nothing for av01 and vp09, which have real decoders there, so
 * the promise resolved, powerEfficient came back false, and retail Windows says
 * true. Measured 2026-08-08 against retail run HEADED - the mode a user's
 * machine is in, and the mode that answers differently: 4 of 8 rows still
 * differed with that hook in place. A declaration that only applies when the
 * engine fails is not a declaration, it is a fallback.
 *
 * Declared here rather than at PDMFactory::Supports because powerEfficient is
 * not a property of the track mime type: vp09 in mp4 reports true and a bare
 * vp9 in webm reports false, and both are video/vp9.
 */
Maybe<MediaCapabilitiesDecodingInfo> StealthDeclaredDecodingInfoFor(
    const MediaDecodingConfiguration& aConfiguration);

}  // namespace dom

}  // namespace mozilla

#endif  // DOM_MEDIA_STEALTHMEDIADECLARATION_H_
