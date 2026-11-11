/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nr_stealth_bridge.h"

#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mozilla/Assertions.h"

// nICEr includes
// clang-format off
#include <csi_platform.h>
#include "nr_api.h"
#include "async_timer.h"
#include "r_memory.h"
#include "transport_addr.h"
#include "local_addr.h"
#include "nr_socket.h"
#include "ice_codeword.h"
#include "ice_ctx.h"
#include "ice_candidate.h"
#include "ice_component.h"
#include "ice_media_stream.h"
// clang-format on

/* UNA SOLA FONTE, e non e' una preferenza.
 *
 * Fino al 2026-08-25 ognuna di queste funzioni leggeva DUE sorgenti - la pref
 * `zoom.stealth.webrtc.*` e la variabile d'ambiente `STEALTHFOX_WEBRTC_*` - e
 * per giunta in ordine diverso l'una dall'altra: `public_ip` provava la pref
 * per prima, `host_ip` e `disable_ipv6` l'ambiente. Tre funzioni sorelle, tre
 * precedenze, e l'intestazione ne dichiarava una quarta ancora diversa da
 * tutte. E' cio' che la regola 7 vieta: due numeri per la stessa cosa, che
 * possono divergere.
 *
 * Nessuno impostava le pref. `invisible_core` dichiara questi valori come
 * ambiente - `build_launch_env` sul percorso diretto, `build_env` su quello
 * Playwright - quindi il ramo pref era gia' morto: rispondeva vuoto e si
 * cadeva sempre sull'ambiente. Qui e' stato TOLTO invece che lasciato come
 * ripiego, perche' un ripiego che non si esercita mai non e' una rete di
 * sicurezza: e' una seconda strada che nessuno controlla.
 *
 * L'ambiente e' anche la sorgente giusta, non solo quella superstite: viene
 * fissato alla creazione del processo ed e' ereditato da OGNI processo figlio,
 * compreso il processo socket dove gira nICEr. Una pref deve invece propagarsi
 * via IPC, ed e' il motivo per cui il codice originale teneva l'ambiente come
 * scorciatoia proprio per quel processo. Stessa ragione per cui il manifest dei
 * font viaggia per ambiente e non per pref.
 *
 * Se la dichiarazione manca si RIFIUTA (si torna 0) e il chiamante non emette
 * il candidato sintetico: non si inventa un default e non si chiede alla
 * macchina qual e' il suo IP vero, che e' esattamente la fuga da evitare.
 *
 * `nr_stealth_get_webrtc_host_ip` non c'e' piu': aveva ZERO chiamanti in tutto
 * l'albero ed era dichiarata "Reserved for Phase 2". La pref omonima resta,
 * perche' la legge `PeerConnectionImpl.cpp`, che non passa da qui.
 */

namespace {

size_t CopyEnvTo(const char* aName, char* aBuf, size_t aBufLen) {
  if (!aBuf || aBufLen == 0) {
    return 0;
  }
  const char* value = getenv(aName);
  if (!value || value[0] == '\0') {
    aBuf[0] = '\0';
    return 0;
  }
  size_t len = strlen(value);
  if (len >= aBufLen) {
    len = aBufLen - 1;
  }
  std::memcpy(aBuf, value, len);
  aBuf[len] = '\0';
  return len;
}

/* Only the ADDRESS: nr_transport_addr_is_wildcard also calls port 0 a wildcard,
   and the port of the document is not what decides the route. */
bool IsWildcardIp(const nr_transport_addr* aAddr) {
  static const unsigned char kZero6[16] = {0};
  switch (aAddr->ip_version) {
    case NR_IPV4:
      return aAddr->u.addr4.sin_addr.s_addr == INADDR_ANY;
    case NR_IPV6:
      return !std::memcmp(aAddr->u.addr6.sin6_addr.s6_addr, kZero6,
                          sizeof(kZero6));
    default:
      return false;
  }
}

}  // namespace

extern "C" int nr_stealth_webrtc_disable_ipv6(void) {
  /* "0" spegne esplicitamente; qualunque altro valore non vuoto accende.
   * Assente = spento: senza dichiarazione non si tocca l'ICE gathering. */
  const char* env = getenv("STEALTHFOX_WEBRTC_DISABLE_IPV6");
  if (env && env[0] != '\0') {
    return (env[0] != '0') ? 1 : 0;
  }
  return 0;
}

extern "C" int nr_stealth_refuse_wildcard_target(
    const nr_transport_addr* aTarget) {
  MOZ_ASSERT(aTarget);
  /* The target is the WILDCARD address whenever the document came through a
   * proxy that resolves the host itself (SOCKS with remote DNS): the socket
   * transport never learns the server's address and reports 0.0.0.0
   * (nsSocketTransport::ResolveHost, mProxyTransparentResolvesHost). What a
   * connected UDP socket to 0.0.0.0 then answers is a property of the KERNEL,
   * not of the network:
   *
   *   Windows  connect() fails, WSAEADDRNOTAVAIL (10049), so we get no default
   *            address and the fallback in nr_ice_set_local_addresses runs.
   *   Linux    connect() SUCCEEDS: a zero destination means "this host", the
   *            route is the loopback and getsockname() reports 127.0.0.1. That
   *            becomes the only gathering address, the UDP host socket cannot
   *            be created on it, and the page gets one TCP host and NO srflx.
   *
   * Measured 2026-10-02 on firefox-34 behind SOCKS, remote https origin:
   * Windows "default-route probe failed ... falling back", host + srflx;
   * Linux "Default address: IP4:127.0.0.1", one TCP host, no srflx, and the
   * detectors call WebRTC blocked. Upstream Firefox does the same on each OS.
   *
   * A wildcard target says nothing about which interface would carry the
   * traffic, so it is refused HERE, on every platform. Windows already got
   * exactly this answer from its kernel, so nothing changes there; Linux
   * stops asking its kernel a question whose answer is a loopback address.
   */
  if (!IsWildcardIp(aTarget)) {
    return 0;
  }
  r_log(LOG_GENERIC, LOG_DEBUG,
        "Default address: target %s is the wildcard (proxy resolves the host), "
        "no default route to probe",
        aTarget->as_string);
  return 1;
}

extern "C" int nr_stealth_ice_pick_addrs(const char* aLabel,
                                        nr_local_addr* aLocalAddrs,
                                        nr_local_addr* aDefaultAddrs,
                                        int aDefaultAddrCt,
                                        nr_local_addr** aAddrs, int* aAddrCt) {
  MOZ_ASSERT(aAddrs && aAddrCt);
  if (aDefaultAddrCt) {
    *aAddrs = aDefaultAddrs;
    *aAddrCt = aDefaultAddrCt;
    return 0;
  }
  /* The default-route probe (a connected UDP socket to the document's remote,
   * see nr_ice_get_default_address in ice_ctx.cpp) can't complete behind a
   * SOCKS proxy: the proxy resolves the host, the document's remote is the
   * wildcard 0.0.0.0, and the probe refuses it (Windows' kernel refused it on
   * its own; Linux' answered 127.0.0.1 until the probe started refusing it
   * explicitly). We get zero default addresses. Failing ICE here blanks WebRTC
   * entirely (host + srflx both "blocked"), which is itself a proxy/stealth
   * tell. If we DO have real filtered local addresses (they are still in
   * aLocalAddrs: NR_ICE_CTX_FLAGS_ONLY_DEFAULT_ADDRS only asks for the route
   * probe's answer in their place, and the probe has none), fall back to them:
   * obfuscate_host_addresses still masks them as <uuid>.local and the
   * synthetic srflx injection then fires, so the page sees a normal NAT-user
   * profile instead of a dead stack. */
  if (*aAddrCt) {
    r_log(LOG_ICE, LOG_WARNING,
          "ICE(%s): default-route probe failed (proxy?); falling back to %d "
          "filtered local addr(s)",
          aLabel, *aAddrCt);
    *aAddrs = aLocalAddrs;
    /* *aAddrCt is left as it is: it already holds the filtered survivor count
     * (nr_stun_filter_addrs and the force-interface pass in
     * nr_ice_set_local_addresses). */
    return 0;
  }
  r_log(LOG_ICE, LOG_ERR, "ICE(%s): failed to find default addresses", aLabel);
  return R_FAILED;
}
