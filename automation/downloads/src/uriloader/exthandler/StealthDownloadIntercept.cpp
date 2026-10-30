/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDownloadIntercept.h"

#include "mozilla/ErrorNames.h"
#include "nsComponentManagerUtils.h"
#include "nsExternalHelperAppService.h"
#include "nsIBackgroundFileSaver.h"
#include "nsIChannel.h"
#include "nsIFile.h"
#include "nsIHttpChannelInternal.h"
#include "nsILoadInfo.h"
#include "nsIURI.h"
#include "nsNetCID.h"

// Same saver setup as nsExternalAppHandler::SetUpTempFile, but the target is
// the file the interceptor chose and it is final: no ".part", no rename.
// static
nsresult StealthDownloadIntercept::CreateSaver(
    nsExternalAppHandler* aHandler) {
  MOZ_ASSERT(!aHandler->mSaver, "Output file initialization called twice!");
  nsresult rv;
  aHandler->mSaver =
      do_CreateInstance(NS_BACKGROUNDFILESAVERSTREAMLISTENER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aHandler->mSaver->SetObserver(aHandler);
  if (NS_FAILED(rv)) {
    aHandler->mSaver = nullptr;
    return rv;
  }

  rv = aHandler->mSaver->EnableSha256();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = aHandler->mSaver->EnableSignatureInfo();
  NS_ENSURE_SUCCESS(rv, rv);

  return aHandler->mSaver->SetTarget(aHandler->mTempFile, false);
}

// static
bool StealthDownloadIntercept::Start(nsExternalAppHandler* aHandler,
                                     nsIRequest* aRequest) {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIDownloadInterceptor> interceptor =
      aHandler->mExtProtSvc->mInterceptor;
  if (!interceptor) {
    return false;
  }

  nsCOMPtr<nsIFile> file;
  bool intercepted = false;
  nsresult rv = interceptor->InterceptDownloadRequest(
      aHandler, aRequest, aHandler->mBrowsingContext, getter_AddRefs(file),
      &intercepted);
  // A throwing interceptor must not break downloads: act as if it declined.
  if (NS_FAILED(rv) || !intercepted) {
    return false;
  }

  // From here the interceptor expects one onDownloadComplete.
  aHandler->mStealthIntercepted = true;

  // Intercepted without a file: the client does not want the data.
  if (!file) {
    aHandler->Cancel(NS_BINDING_ABORTED);
    return true;
  }

  // The file is the final destination, and no nsITransfer is created, which
  // keeps the downloads UI and its JS components out of this download.
  aHandler->mTempFile = file;
  rv = file->GetLeafName(aHandler->mTempLeafName);
  if (NS_SUCCEEDED(rv)) {
    rv = CreateSaver(aHandler);
  }
  if (NS_FAILED(rv)) {
    aRequest->Cancel(rv);
    aHandler->Cancel(rv);
    return true;
  }

  // Same channel preparation OnStartRequest does after SetUpTempFile.
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(aRequest);
  nsCOMPtr<nsIHttpChannelInternal> httpInternal = do_QueryInterface(channel);
  if (httpInternal) {
    (void)httpInternal->SetChannelIsForDownload(true);
  }
  if (channel && aHandler->mSourceUrl &&
      aHandler->mSourceUrl->SchemeIs("data")) {
    nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
    loadInfo->SetForceAllowDataURI(true);
  }
  return true;
}

// static
void StealthDownloadIntercept::Complete(nsExternalAppHandler* aHandler,
                                        nsresult aStatus) {
  // The interceptor may drop its last reference to the handler.
  RefPtr<nsExternalAppHandler> kungFuDeathGrip(aHandler);

  // Cleared first: Cancel and OnSaveComplete both end here, only one reports.
  if (!aHandler->mStealthIntercepted) {
    return;
  }
  aHandler->mStealthIntercepted = false;

  nsCOMPtr<nsIDownloadInterceptor> interceptor =
      aHandler->mExtProtSvc->mInterceptor;
  if (!interceptor) {
    return;
  }

  nsAutoCString errorName;
  if (NS_FAILED(aStatus)) {
    mozilla::GetErrorName(aStatus, errorName);
  }
  (void)interceptor->OnDownloadComplete(aHandler, errorName);
}
