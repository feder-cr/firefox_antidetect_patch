/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nr_stealth_bridge.h"

#include <cstring>
#include "mozilla/StaticPrefs_zoom.h"
#include "nsString.h"

namespace {

template <typename Lock>
size_t CopyLockedStringTo(Lock& aLock, char* aBuf, size_t aBufLen) {
  if (!aBuf || aBufLen == 0) {
    return 0;
  }
  const nsCString& value = *aLock;
  if (value.IsEmpty()) {
    aBuf[0] = '\0';
    return 0;
  }
  size_t toCopy = value.Length();
  if (toCopy >= aBufLen) {
    toCopy = aBufLen - 1;
  }
  std::memcpy(aBuf, value.Data(), toCopy);
  aBuf[toCopy] = '\0';
  return toCopy;
}

}  // namespace

extern "C" size_t nr_stealth_get_webrtc_public_ip(char* buf, size_t buf_len) {
  /* Pref first — updated per stealthfox_new_session, mirrored to all
   * processes via mirror:always. Env var is fallback only (e.g. for the
   * socket process where StaticPref propagation timing might be unreliable).
   */
  auto lock = mozilla::StaticPrefs::zoom_stealth_webrtc_public_ip();
  size_t pref_len = CopyLockedStringTo(lock, buf, buf_len);
  if (pref_len > 0) return pref_len;

  const char* env = getenv("STEALTHFOX_WEBRTC_PUBLIC_IP");
  if (env && env[0] != '\0') {
    if (!buf || buf_len == 0) return 0;
    size_t len = strlen(env);
    if (len >= buf_len) len = buf_len - 1;
    std::memcpy(buf, env, len);
    buf[len] = '\0';
    return len;
  }
  return 0;
}

extern "C" size_t nr_stealth_get_webrtc_host_ip(char* buf, size_t buf_len) {
  const char* env = getenv("STEALTHFOX_WEBRTC_HOST_IP");
  if (env && env[0] != '\0') {
    if (!buf || buf_len == 0) return 0;
    size_t len = strlen(env);
    if (len >= buf_len) len = buf_len - 1;
    std::memcpy(buf, env, len);
    buf[len] = '\0';
    return len;
  }
  auto lock = mozilla::StaticPrefs::zoom_stealth_webrtc_host_ip();
  return CopyLockedStringTo(lock, buf, buf_len);
}

extern "C" int nr_stealth_webrtc_disable_ipv6(void) {
  /* Env var first (read directly in the socket process, no IPC timing race —
   * same rationale as the public IP). "0" disables explicitly; any other
   * non-empty value enables. Falls back to the pref when the env is unset. */
  const char* env = getenv("STEALTHFOX_WEBRTC_DISABLE_IPV6");
  if (env && env[0] != '\0') {
    return (env[0] != '0') ? 1 : 0;
  }
  return mozilla::StaticPrefs::zoom_stealth_webrtc_disable_ipv6() ? 1 : 0;
}
