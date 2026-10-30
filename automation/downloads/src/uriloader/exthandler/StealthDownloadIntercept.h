/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthDownloadIntercept_h_
#define StealthDownloadIntercept_h_

#include "nscore.h"

class nsExternalAppHandler;
class nsIRequest;

/**
 * Stealthfox (automation/downloads): lets our automation's download
 * interceptor (nsIDownloadInterceptor, installed with
 * nsIExternalHelperAppService.setDownloadInterceptor) take over a download.
 * Parent process, main thread only. Friend of nsExternalAppHandler, which
 * only calls Start and Complete.
 */
class StealthDownloadIntercept final {
 public:
  /**
   * Called from nsExternalAppHandler::OnStartRequest, where it would set up
   * its own temp file. Asks the interceptor about this download. Returns true
   * when the interceptor took it: the data goes to the file it chose (or the
   * download is canceled when it chose none) and the caller must return
   * NS_OK, skipping the transfer, the dialogs and the downloads UI. Returns
   * false (nothing changed) when there is no interceptor, it declined or it
   * failed, and the caller carries on exactly as stock Firefox.
   */
  static bool Start(nsExternalAppHandler* aHandler, nsIRequest* aRequest);

  /**
   * Called when the handler's download ended (saved, canceled or failed).
   * Tells the interceptor once per intercepted download, with the error name
   * (empty on success). Does nothing for downloads that were not intercepted.
   */
  static void Complete(nsExternalAppHandler* aHandler, nsresult aStatus);

 private:
  static nsresult CreateSaver(nsExternalAppHandler* aHandler);
};

#endif  // StealthDownloadIntercept_h_
