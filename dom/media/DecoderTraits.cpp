/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DecoderTraits.h"

#include "MediaContainerType.h"
#include "OggDecoder.h"
#include "OggDemuxer.h"
#include "WebMDecoder.h"
#include "WebMDemuxer.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/glean/DomMediaHlsMetrics.h"
#include "mozilla/glean/DomMediaMetrics.h"
#include "nsMimeTypes.h"

#ifdef MOZ_ANDROID_HLS_SUPPORT
#  include "HLSDecoder.h"
#endif
#include "ADTSDecoder.h"
#include "ADTSDemuxer.h"
#include "FlacDecoder.h"
#include "FlacDemuxer.h"
#include "MP3Decoder.h"
#include "MP3Demuxer.h"
#include "MP4Decoder.h"
#include "MP4Demuxer.h"
#include "MatroskaDecoder.h"
#include "MatroskaDemuxer.h"
#include "MediaFormatReader.h"
#include "WaveDecoder.h"
#include "WaveDemuxer.h"

namespace mozilla {

extern LazyLogModule gMediaDecoderLog;
#define LOGD(x, ...) \
  MOZ_LOG_FMT(gMediaDecoderLog, LogLevel::Debug, x, ##__VA_ARGS__)

/* static */
bool DecoderTraits::IsHttpLiveStreamingType(const MediaContainerType& aType) {
  const auto& mimeType = aType.Type();
  return  // For m3u8.
          // https://tools.ietf.org/html/draft-pantos-http-live-streaming-19#section-10
      mimeType == MEDIAMIMETYPE("application/vnd.apple.mpegurl") ||
      // Some sites serve these as the informal m3u type.
      mimeType == MEDIAMIMETYPE("application/x-mpegurl") ||
      mimeType == MEDIAMIMETYPE("audio/mpegurl") ||
      mimeType == MEDIAMIMETYPE("audio/x-mpegurl");
}

static CanPlayStatus CanHandleCodecsType(
    const MediaContainerType& aType, DecoderDoctorDiagnostics* aDiagnostics) {
  // We should have been given a codecs string, though it may be empty.
  MOZ_ASSERT(aType.ExtendedType().HaveCodecs());

  // Container type with the MIME type, no codecs.
  const MediaContainerType mimeType(aType.Type());

  if (OggDecoder::IsSupportedType(mimeType)) {
    if (OggDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // ogg is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (WaveDecoder::IsSupportedType(MediaContainerType(mimeType))) {
    if (WaveDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested, wave
    // is supported and working: the codec must be invalid or not supported.
    return CANPLAY_NO;
  }
  if (WebMDecoder::IsSupportedType(mimeType)) {
    if (WebMDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // webm is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (MP4Decoder::IsSupportedType(mimeType,
                                  /* DecoderDoctorDiagnostics* */ nullptr)) {
    if (MP4Decoder::IsSupportedType(aType, aDiagnostics)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // fmp4 is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (MP3Decoder::IsSupportedType(mimeType)) {
    if (MP3Decoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // mp3 is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (ADTSDecoder::IsSupportedType(mimeType)) {
    if (ADTSDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // adts is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (FlacDecoder::IsSupportedType(mimeType)) {
    if (FlacDecoder::IsSupportedType(aType)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // flac is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }
  if (MatroskaDecoder::IsSupportedType(
          mimeType,
          /* DecoderDoctorDiagnostics* */ nullptr)) {
    if (MatroskaDecoder::IsSupportedType(aType, aDiagnostics)) {
      return CANPLAY_YES;
    }
    // We can only reach this position if a particular codec was requested,
    // mkv is supported and working: the codec must be invalid.
    return CANPLAY_NO;
  }

  return CANPLAY_MAYBE;
}

// Stealth: the answer a page gets for a media type is DECLARED, not derived
// from what this build happens to be able to decode.
//
// The engine's own answer is host-dependent, not merely OS-dependent, which is
// the worse of the two. H.264 is the case: the Linux build ships the ffmpeg
// platform decoder but the bundled ffvpx has H.264 compiled out, so the answer
// depends on whether the USER'S machine has libavcodec - two Linux users
// distinguish themselves from each other. Windows answers "probably" through
// WMF. Measured 2026-08-08, one seed on both hosts, 14 media types probed four
// ways each: exactly the three avc1 rows differed.
//
// Consulted BEFORE any decoder is asked - blocking the engine at birth rather
// than correcting its answer, the same shape as the font manifest and the
// coverage ladder. A type the table does not name falls through untouched:
// this overrides a declared list, it does not replace the media stack.
//
// THE COST, stated because it is a trade and not a win: a declared "probably"
// for a codec this build cannot decode means a site picks H.264 and the video
// does not play, where today it falls back to WebM and plays. Fidelity to what
// a Windows Firefox reports is bought with a broken playback on Linux. The way
// out is to ship the decoder, not to change this answer back.
static bool StealthDeclaredCanPlay(const MediaContainerType& aType,
                                   CanPlayStatus& aOut) {
  nsAutoCString spec;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_media_mime_answers();
    spec = *lock;
  }
  if (spec.IsEmpty()) {
    return false;
  }
  const nsCString want(aType.OriginalString());
  // NEWLINE-separated, and it has to be. The separator used to be a comma,
  // which is also what a multi-codec type carries inside its own value, so
  // `video/mp4; codecs="avc1.42E01E, mp4a.40.2"` was split into two fragments,
  // neither of which matched anything, and the entry silently did nothing. The
  // format could not express the commonest shape in the wild, because every
  // real video declares codecs="<video>, <audio>". Found 2026-08-08 by
  // measuring a 62-type corpus against retail: it was the last of 8 cross-OS
  // divergences left after the table was filled, and it survived being ADDED
  // to the table, which is what pointed at the parser rather than the data.
  for (const nsACString& entry : spec.Split('\n')) {
    const int32_t bar = nsCString(entry).FindChar('|');
    if (bar <= 0) {
      continue;
    }
    if (!want.Equals(Substring(entry, 0, bar))) {
      continue;
    }
    const nsDependentCSubstring status(Substring(entry, bar + 1));
    if (status.EqualsLiteral("yes")) {
      aOut = CANPLAY_YES;
    } else if (status.EqualsLiteral("maybe")) {
      aOut = CANPLAY_MAYBE;
    } else if (status.EqualsLiteral("no")) {
      aOut = CANPLAY_NO;
    } else {
      // An UNRECOGNISED token falls through to the real logic; it does not mean
      // "no". Found by audit 2026-08-08: the else-branch used to answer
      // CANPLAY_NO for anything it did not recognise, so one typo in the pref -
      // "probably" instead of "yes", say - would have silently told every page
      // that a codec this build decodes perfectly well is unsupported. That
      // inverts rule 7: this is an override list, and the failure mode has to
      // stay "we fail to disguise", never "the page breaks".
      return false;
    }
    return true;
  }
  return false;
}

static CanPlayStatus CanHandleMediaType(
    const MediaContainerType& aType, DecoderDoctorDiagnostics* aDiagnostics) {
  CanPlayStatus declared;
  if (StealthDeclaredCanPlay(aType, declared)) {
    return declared;
  }
  if (DecoderTraits::IsHttpLiveStreamingType(aType)) {
    glean::hls::canplay_requested.Add();
  }
  if (MatroskaDecoder::IsMatroskaType(aType)) {
    glean::media::mkv_content_count.Add();
  }
#ifdef MOZ_ANDROID_HLS_SUPPORT
  if (HLSDecoder::IsSupportedType(aType)) {
    glean::hls::canplay_supported.Add();
    return CANPLAY_MAYBE;
  }
#endif

  if (aType.ExtendedType().HaveCodecs()) {
    CanPlayStatus result = CanHandleCodecsType(aType, aDiagnostics);
    if (result == CANPLAY_NO || result == CANPLAY_YES) {
      return result;
    }
  }

  // Container type with just the MIME type/subtype, no codecs.
  const MediaContainerType mimeType(aType.Type());

  if (OggDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (WaveDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (MP4Decoder::IsSupportedType(mimeType, aDiagnostics)) {
    return CANPLAY_MAYBE;
  }
  if (WebMDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (MP3Decoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (ADTSDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (FlacDecoder::IsSupportedType(mimeType)) {
    return CANPLAY_MAYBE;
  }
  if (MatroskaDecoder::IsSupportedType(mimeType, aDiagnostics)) {
    return CANPLAY_MAYBE;
  }
  return CANPLAY_NO;
}

/* static */
CanPlayStatus DecoderTraits::CanHandleContainerType(
    const MediaContainerType& aContainerType,
    DecoderDoctorDiagnostics* aDiagnostics) {
  return CanHandleMediaType(aContainerType, aDiagnostics);
}

/* static */
bool DecoderTraits::ShouldHandleMediaType(
    const nsACString& aMIMEType, DecoderDoctorDiagnostics* aDiagnostics) {
  Maybe<MediaContainerType> containerType = MakeMediaContainerType(aMIMEType);
  if (!containerType) {
    return false;
  }

  if (WaveDecoder::IsSupportedType(*containerType)) {
    // We should not return true for Wave types, since there are some
    // Wave codecs actually in use in the wild that we don't support, and
    // we should allow those to be handled by plugins or helper apps.
    // Furthermore people can play Wave files on most platforms by other
    // means.
    return false;
  }

  return CanHandleMediaType(*containerType, aDiagnostics) != CANPLAY_NO;
}

/* static */
already_AddRefed<MediaDataDemuxer> DecoderTraits::CreateDemuxer(
    const MediaContainerType& aType, MediaResource* aResource) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<MediaDataDemuxer> demuxer;

  if (MP4Decoder::IsSupportedType(aType,
                                  /* DecoderDoctorDiagnostics* */ nullptr)) {
    demuxer = new MP4Demuxer(aResource);
  } else if (MP3Decoder::IsSupportedType(aType)) {
    demuxer = new MP3Demuxer(aResource);
  } else if (ADTSDecoder::IsSupportedType(aType)) {
    demuxer = new ADTSDemuxer(aResource);
  } else if (WaveDecoder::IsSupportedType(aType)) {
    demuxer = new WAVDemuxer(aResource);
  } else if (FlacDecoder::IsSupportedType(aType)) {
    demuxer = new FlacDemuxer(aResource);
  } else if (OggDecoder::IsSupportedType(aType)) {
    demuxer = new OggDemuxer(aResource);
  } else if (WebMDecoder::IsSupportedType(aType)) {
    demuxer = new WebMDemuxer(aResource);
  } else if (MatroskaDecoder::IsSupportedType(
                 aType,
                 /* DecoderDoctorDiagnostics* */ nullptr)) {
    demuxer = new MatroskaDemuxer(aResource);
  } else {
    LOGD("CreateDemuxer: unsupported type {}", aType.OriginalString().get());
  }

  return demuxer.forget();
}

/* static */
MediaFormatReader* DecoderTraits::CreateReader(const MediaContainerType& aType,
                                               MediaFormatReaderInit& aInit) {
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<MediaDataDemuxer> demuxer = CreateDemuxer(aType, aInit.mResource);
  if (!demuxer) {
    return nullptr;
  }

  MediaFormatReader* decoderReader = new MediaFormatReader(aInit, demuxer);

  if (OggDecoder::IsSupportedType(aType)) {
    static_cast<OggDemuxer*>(demuxer.get())
        ->SetChainingEvents(&decoderReader->TimedMetadataProducer(),
                            &decoderReader->MediaNotSeekableProducer());
  }

  return decoderReader;
}

/* static */
bool DecoderTraits::IsSupportedInVideoDocument(const nsACString& aType) {
  // Forbid playing media in video documents if the user has opted
  // not to, using either the legacy WMF specific pref, or the newer
  // catch-all pref.
  if (!Preferences::GetBool("media.wmf.play-stand-alone", true) ||
      !Preferences::GetBool("media.play-stand-alone", true)) {
    return false;
  }

  Maybe<MediaContainerType> type = MakeMediaContainerType(aType);
  if (!type) {
    return false;
  }

  return OggDecoder::IsSupportedType(*type) ||
         WebMDecoder::IsSupportedType(*type) ||
         MP4Decoder::IsSupportedType(*type,
                                     /* DecoderDoctorDiagnostics* */ nullptr) ||
         MP3Decoder::IsSupportedType(*type) ||
         ADTSDecoder::IsSupportedType(*type) ||
         FlacDecoder::IsSupportedType(*type) ||
         MatroskaDecoder::IsSupportedType(
             *type,
             /* DecoderDoctorDiagnostics* */ nullptr) ||
#ifdef MOZ_ANDROID_HLS_SUPPORT
         HLSDecoder::IsSupportedType(*type) ||
#endif
         false;
}

/* static */
nsTArray<UniquePtr<TrackInfo>> DecoderTraits::GetTracksInfo(
    const MediaContainerType& aType) {
  // Container type with just the MIME type/subtype, no codecs.
  const MediaContainerType mimeType(aType.Type());

  if (OggDecoder::IsSupportedType(mimeType)) {
    return OggDecoder::GetTracksInfo(aType);
  }
  if (WaveDecoder::IsSupportedType(mimeType)) {
    return WaveDecoder::GetTracksInfo(aType);
  }
  if (MP4Decoder::IsSupportedType(mimeType, nullptr)) {
    return MP4Decoder::GetTracksInfo(aType);
  }
  if (WebMDecoder::IsSupportedType(mimeType)) {
    return WebMDecoder::GetTracksInfo(aType);
  }
  if (MP3Decoder::IsSupportedType(mimeType)) {
    return MP3Decoder::GetTracksInfo(aType);
  }
  if (ADTSDecoder::IsSupportedType(mimeType)) {
    return ADTSDecoder::GetTracksInfo(aType);
  }
  if (FlacDecoder::IsSupportedType(mimeType)) {
    return FlacDecoder::GetTracksInfo(aType);
  }
  if (MatroskaDecoder::IsSupportedType(mimeType, nullptr)) {
    return MatroskaDecoder::GetTracksInfo(aType);
  }
  return nsTArray<UniquePtr<TrackInfo>>();
}

}  // namespace mozilla

// avoid redefined macro in unified build
#undef LOGD
