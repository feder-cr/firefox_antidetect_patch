/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsScreencastService.h"

#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <memory>
#include <utility>

#include "VideoEngine.h"
#include "video_engine/desktop_capture_impl.h"

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"
#include "modules/video_capture/video_capture_defines.h"

#include "gfxPlatform.h"
#include "mozilla/Base64.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/PresShell.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/gfx/Rect.h"
#include "nsIDocShell.h"
#include "nsIRandomGenerator.h"
#include "nsIWidget.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"

extern "C" {
#include "jpeglib.h"
}
#include "libyuv/convert_argb.h"

namespace mozilla {

NS_IMPL_ISUPPORTS(nsScreencastService, nsIScreencastService)

namespace {

// Flow control. A client that cannot keep up sees fewer frames; the queue to
// the main thread never grows, and neither does the base64 held in memory.
const uint32_t kMaxFramesInFlight = 1;

StaticRefPtr<nsScreencastService> gScreencastService;

nsresult GenerateUid(nsString& aUid) {
  nsresult rv = NS_OK;
  nsCOMPtr<nsIRandomGenerator> rg =
      do_GetService("@mozilla.org/security/random-generator;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  uint8_t* buffer = nullptr;
  const int kLen = 16;
  rv = rg->GenerateRandomBytes(kLen, &buffer);
  NS_ENSURE_SUCCESS(rv, rv);
  for (int i = 0; i < kLen; i++) {
    aUid.AppendPrintf("%02x", buffer[i]);
  }
  free(buffer);
  return rv;
}

}  // namespace

// One capture session: one window, one capture module, one client.
//
// The module is Firefox's own DesktopCaptureImpl, the code path behind
// getDisplayMedia's window sharing. It hands us I420 frames on its capture
// thread; we crop and scale in one step, convert to ARGB, encode a JPEG,
// base64 it and hop to the main thread to give the string to the JavaScript
// client. Nothing here touches the content process or the page.
class nsScreencastService::Session final
    : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(Session)

  Session(nsIScreencastServiceClient* aClient, nsIWidget* aWidget,
          webrtc::scoped_refptr<webrtc::DesktopCaptureImpl> aModule,
          int aWidth, int aHeight, int aViewportWidth, int aViewportHeight,
          gfx::IntMargin aMargin, uint32_t aQuality, uint32_t aFps)
      : mClient(aClient),
        mWidget(aWidget),
        mModule(std::move(aModule)),
        mWidth(aWidth),
        mHeight(aHeight),
        mViewportWidth(aViewportWidth),
        mViewportHeight(aViewportHeight),
        mMargin(aMargin),
        mQuality(aQuality),
        mFps(aFps) {}

  nsIWidget* Widget() const { return mWidget; }

  // Main thread. The module was created on this thread, and DesktopCaptureImpl
  // asserts that StartCapture and StopCapture come from the thread that
  // created it.
  bool Start() {
    webrtc::VideoCaptureCapability capability;
    // The desktop capturer takes its size from the window; the frame rate is
    // the only knob it reads here.
    capability.width = 1280;
    capability.height = 960;
    capability.maxFPS = static_cast<int32_t>(mFps);
    capability.videoType = webrtc::VideoType::kI420;
    mModule->RegisterCaptureDataCallback(this);
    if (mModule->StartCapture(capability) != 0) {
      mModule->DeRegisterCaptureDataCallback();
      fprintf(stderr, "screencast: StartCapture failed\n");
      return false;
    }
    return true;
  }

  // Main thread.
  //
  // The order is load-bearing. DeRegisterCaptureDataCallback takes the same
  // lock NotifyOnFrame holds while it is inside OnFrame, so when it returns
  // no OnFrame is running and none will start; StopCapture then joins the
  // capture thread. A frame already queued to the main thread finds mStopped
  // set and drops itself.
  void Stop() {
    if (mStopped.exchange(true)) {
      return;
    }
    mModule->DeRegisterCaptureDataCallback();
    mModule->StopCapture();
    if (mClient) {
      mClient->ScreencastStopped();
      mClient = nullptr;
    }
  }

  void Ack() {
    if (mFramesInFlight.load() == 0) {
      return;
    }
    mFramesInFlight.fetch_sub(1);
  }

  // Capture thread.
  void OnFrame(const webrtc::VideoFrame& aFrame) override {
    if (mStopped.load() || mFramesInFlight.load() >= kMaxFramesInFlight) {
      return;
    }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        aFrame.video_frame_buffer()->ToI420();
    if (!i420) {
      return;
    }
    const int frameWidth = i420->width();
    const int frameHeight = i420->height();

    // The area we keep: the window, minus its OS frame and the toolbar rows
    // above the content - unless the caller asked for the whole window, in
    // which case the margin is zero and this is the identity.
    //
    // ⛔ The margin's members are `IntCoordTyped`, a unit-carrying type, not
    // int. std::clamp deduces one type from all three arguments and refuses
    // the mix, so the conversion is written here once rather than left to
    // four call sites to get right.
    const int marginLeft = int(mMargin.left);
    const int marginTop = int(mMargin.top);
    const int marginRight = int(mMargin.right);
    const int marginBottom = int(mMargin.bottom);
    const int left = std::clamp(marginLeft, 0, std::max(frameWidth - 1, 0));
    const int top = std::clamp(marginTop, 0, std::max(frameHeight - 1, 0));
    int cropWidth = frameWidth - left - std::max(marginRight, 0);
    int cropHeight = frameHeight - top - std::max(marginBottom, 0);
    // A minimized window is captured as 1x1.
    if (cropWidth <= 1 || cropHeight <= 1) {
      return;
    }
    // A headed window brings its size in sync with the viewport slowly.
    if (mViewportWidth && cropWidth > mViewportWidth) {
      cropWidth = mViewportWidth;
    }
    if (mViewportHeight && cropHeight > mViewportHeight) {
      cropHeight = mViewportHeight;
    }
    // CropAndScaleFrom asserts these with RTC_CHECK, which aborts the parent
    // process. A frame we cannot describe is dropped instead.
    if (left + cropWidth > frameWidth || top + cropHeight > frameHeight) {
      return;
    }

    // Scale down to fit the requested bound, never up.
    double scale = 1.0;
    if (mWidth < cropWidth || mHeight < cropHeight) {
      scale = std::min(1.0, std::min(double(mWidth) / cropWidth,
                                     double(mHeight) / cropHeight));
    }
    const int outWidth = std::max(1, int(cropWidth * scale));
    const int outHeight = std::max(1, int(cropHeight * scale));

    // One step: the crop and the scale together, straight into a buffer that
    // is already the output size. Nothing outside the kept area is ever
    // converted, which is the half of the work Playwright's version did and
    // then threw away.
    webrtc::scoped_refptr<webrtc::I420Buffer> out =
        webrtc::I420Buffer::Create(outWidth, outHeight);
    if (!out) {
      return;
    }
    out->CropAndScaleFrom(*i420, left, top, cropWidth, cropHeight);

    // I420 to ARGB. libyuv's "ARGB" is B,G,R,A in memory on little-endian.
    const int stride = outWidth * 4;
    std::unique_ptr<uint8_t[]> argb(new uint8_t[size_t(stride) * outHeight]);
    if (libyuv::I420ToARGB(out->DataY(), out->StrideY(), out->DataU(),
                           out->StrideU(), out->DataV(), out->StrideV(),
                           argb.get(), stride, outWidth, outHeight) != 0) {
      return;
    }

    jpeg_compress_struct info;
    jpeg_error_mgr error;
    info.err = jpeg_std_error(&error);
    jpeg_create_compress(&info);
    unsigned char* jpegBuffer = nullptr;
    unsigned long jpegSize = 0;
    jpeg_mem_dest(&info, &jpegBuffer, &jpegSize);
    info.image_width = outWidth;
    info.image_height = outHeight;
    info.input_components = 4;
    // libyuv's "ARGB" is a 32-bit little-endian word, so the bytes come out
    // B,G,R,A on a little-endian machine and A,R,G,B on a big-endian one.
    //
    // ⛔ `if constexpr`, not `#if MOZ_LITTLE_ENDIAN()`. That macro is what
    // Playwright's version used and it does NOT EXIST in Firefox 151 - the
    // tree moved to std::endian, and the preprocessor form fails the build
    // with "function-like macro is not defined" rather than quietly picking
    // the wrong branch. Both branches here are compiled on every platform, so
    // the one this machine does not take cannot rot unnoticed.
    if constexpr (std::endian::native == std::endian::little) {
      info.in_color_space = JCS_EXT_BGRA;
    } else {
      info.in_color_space = JCS_EXT_ARGB;
    }
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, int(mQuality), true);
    jpeg_start_compress(&info, true);
    while (info.next_scanline < info.image_height) {
      JSAMPROW row = argb.get() + size_t(info.next_scanline) * size_t(stride);
      if (jpeg_write_scanlines(&info, &row, 1) != 1) {
        fprintf(stderr, "screencast: the JPEG encoder refused a scanline\n");
        break;
      }
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);

    nsCString base64;
    nsresult rv = Base64Encode(reinterpret_cast<const char*>(jpegBuffer),
                               uint32_t(jpegSize), base64);
    free(jpegBuffer);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return;
    }

    mFramesInFlight.fetch_add(1);
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "nsScreencastService::Session::Frame",
        [self = RefPtr{this}, base64 = std::move(base64), cropWidth,
         cropHeight]() {
          if (self->mStopped.load() || !self->mClient) {
            return;
          }
          NS_ConvertUTF8toUTF16 utf16(base64);
          self->mClient->ScreencastFrame(utf16, uint32_t(cropWidth),
                                         uint32_t(cropHeight));
        }));
  }

 private:
  ~Session() override = default;

  // Main thread only: the client is a JavaScript object.
  nsCOMPtr<nsIScreencastServiceClient> mClient;
  nsCOMPtr<nsIWidget> mWidget;
  webrtc::scoped_refptr<webrtc::DesktopCaptureImpl> mModule;
  const int mWidth;
  const int mHeight;
  const int mViewportWidth;
  const int mViewportHeight;
  const gfx::IntMargin mMargin;
  const uint32_t mQuality;
  const uint32_t mFps;
  std::atomic<bool> mStopped{false};
  std::atomic<uint32_t> mFramesInFlight{0};
};

// static
already_AddRefed<nsIScreencastService> nsScreencastService::GetSingleton() {
  if (!gScreencastService) {
    gScreencastService = new nsScreencastService();
    ClearOnShutdown(&gScreencastService);
  }
  return do_AddRef(gScreencastService);
}

nsScreencastService::nsScreencastService() = default;

nsScreencastService::~nsScreencastService() {
  // A session outliving the service would leave a capture thread running and
  // a window being read after nobody is listening.
  for (auto& it : mIdToSession) {
    it.second->Stop();
  }
  mIdToSession.clear();
}

NS_IMETHODIMP nsScreencastService::StartScreencast(
    nsIScreencastServiceClient* aClient, nsIDocShell* aDocShell,
    uint32_t aWidth, uint32_t aHeight, uint32_t aQuality,
    uint32_t aViewportWidth, uint32_t aViewportHeight, uint32_t aOffsetTop,
    bool aFullWindow, uint32_t aFps, nsAString& aSessionId) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread(),
                     "the screencast is started on the main thread");
  if (!aClient || !aDocShell || !aWidth || !aHeight || !aFps) {
    return NS_ERROR_INVALID_ARG;
  }
  // No native window, nothing to capture. Said plainly, instead of a stream
  // of nothing: HeadlessWidget::GetNativeData returns null for every request.
  if (gfxPlatform::IsHeadless()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // The widget of the window this docShell paints into.
  //
  // Not nsIBaseWindow::GetMainWidget: nsDocShell answers that one with its
  // PARENT widget, which is a different question. And not the view tree
  // either - Playwright's version walked PresShell to nsViewManager to
  // nsView, and Firefox 151 has no `view/` directory at all. PresShell
  // climbs the pres contexts to the nearest owning widget, which is the one
  // that was wanted all along.
  PresShell* presShell = aDocShell->GetPresShell();
  if (!presShell) {
    return NS_ERROR_UNEXPECTED;
  }
  nsCOMPtr<nsIWidget> widget = presShell->GetRootWidget();
  if (!widget) {
    return NS_ERROR_UNEXPECTED;
  }
  // The native window id belongs to the top level, and a capture of a child
  // widget would miss the tab strip and the address bar - which is exactly
  // what fullWindow exists to keep.
  if (nsIWidget* topLevel = widget->GetTopLevelWidget()) {
    widget = topLevel;
  }

  // The capture module holds ONE sink, so one session per window. Two pages
  // in one window share the window, and a second screencast there is refused
  // rather than silently stealing the first one's frames.
  for (auto& it : mIdToSession) {
    if (it.second->Widget() == widget) {
      return NS_ERROR_ALREADY_INITIALIZED;
    }
  }

  // What to crop off. Screen bounds are the widget on the screen; client
  // bounds are the content area in the parent's coordinates, so they are
  // walked up to screen coordinates before subtracting. The difference is the
  // window's OS frame.
  //
  // ⛔ THE FRAME IS CROPPED IN BOTH MODES, and the first version of this file
  // did not: with fullWindow it left the margin at zero, and the frames opened
  // from the firefox-28 build carried a strip of a few pixels on the right
  // and at the bottom showing whatever window lay UNDERNEATH. Windows 11
  // draws its resize border transparent on screen, but the capturer hands
  // those pixels over like any others. Nobody sitting at the machine sees
  // that border, so a "full window" frame should not either. What fullWindow
  // skips is only the CHROME crop below: the tab strip, the address bar and
  // the chrome-side pointer overlay are what it exists to keep.
  gfx::IntMargin margin;
  {
    auto screenBounds = widget->GetScreenBounds().ToUnknownRect();
    auto clientBounds = widget->GetClientBounds().ToUnknownRect();
    for (auto parent = widget->GetParent(); parent;
         parent = parent->GetParent()) {
      auto parentBounds = parent->GetClientBounds().ToUnknownRect();
      clientBounds.MoveBy(parentBounds.X(), parentBounds.Y());
    }
    margin = screenBounds - clientBounds;
  }
  if (!aFullWindow) {
    margin.top += int32_t(aOffsetTop);
  }

  uintptr_t rawWindowId = reinterpret_cast<uintptr_t>(
      widget->GetNativeData(NS_NATIVE_WINDOW_WEBRTC_DEVICE_ID));
  if (!rawWindowId) {
    fprintf(stderr, "screencast: this widget has no native window id\n");
    return NS_ERROR_NOT_AVAILABLE;
  }
  nsCString windowId;
  windowId.AppendPrintf("%" PRIuPTR, rawWindowId);

  static int32_t sModuleId = 0;
  // Explicit, because scoped_refptr's constructor from a raw pointer is:
  // taking a reference is a decision, not something a conversion should do
  // quietly. `video_capture_factory.cc` wraps the same call the same way.
  webrtc::scoped_refptr<webrtc::DesktopCaptureImpl> module(
      webrtc::DesktopCaptureImpl::Create(++sModuleId, windowId.get(),
                                         camera::CaptureDeviceType::Window));
  if (!module) {
    return NS_ERROR_FAILURE;
  }

  nsString uid;
  nsresult rv = GenerateUid(uid);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<Session> session = new Session(
      aClient, widget, std::move(module), int(aWidth), int(aHeight),
      int(aViewportWidth), int(aViewportHeight), margin, aQuality, aFps);
  if (!session->Start()) {
    return NS_ERROR_FAILURE;
  }
  aSessionId = uid;
  mIdToSession.emplace(uid, std::move(session));
  return NS_OK;
}

NS_IMETHODIMP nsScreencastService::StopScreencast(
    const nsAString& aSessionId) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread(),
                     "the screencast is stopped on the main thread");
  nsString sessionId(aSessionId);
  auto it = mIdToSession.find(sessionId);
  if (it == mIdToSession.end()) {
    return NS_ERROR_INVALID_ARG;
  }
  RefPtr<Session> session = it->second;
  mIdToSession.erase(it);
  session->Stop();
  return NS_OK;
}

NS_IMETHODIMP nsScreencastService::ScreencastFrameAck(
    const nsAString& aSessionId) {
  nsString sessionId(aSessionId);
  auto it = mIdToSession.find(sessionId);
  if (it == mIdToSession.end()) {
    return NS_ERROR_INVALID_ARG;
  }
  it->second->Ack();
  return NS_OK;
}

}  // namespace mozilla
