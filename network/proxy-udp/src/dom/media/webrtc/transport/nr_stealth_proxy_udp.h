/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Proxy UDP egress: refuse the DIRECT send of an ICE UDP packet to a public
 * destination when the engine runs behind a proxy.
 *
 * Header-only on purpose: transport_lcppsrcs (common.build) is compiled with
 * SOURCES (build/moz.build:40), so a new .cpp would need a new line in
 * common.build, which raises a hook budget. Included only by
 * nr_socket_prsock.cpp. Pref: zoom.stealth.webrtc.no_direct_udp. */

#ifndef nr_stealth_proxy_udp_h__
#define nr_stealth_proxy_udp_h__

#include <stdint.h>

#include "mozilla/StaticPrefs_zoom.h"

// nICEr / nrappkit includes. r_log, LOG_GENERIC, LOG_INFO and R_REJECTED come
// from nr_api.h (r_log.h, r_common.h -> r_errors.h); nr_transport_addr, NR_IPV4,
// NR_IPV6 and ntohl (winsock2.h / netinet/in.h) from transport_addr.h.
#include "nr_api.h"
#include "transport_addr.h"

/* Un pacchetto UDP di ICE puo' uscire DIRETTAMENTE anche dietro un proxy, e
 * chi riceve vede l'indirizzo VERO della macchina.
 *
 * MISURATO il 2026-08-26 su tre sistemi studiati e due schemi. Con uno STUN
 * scritto come INDIRIZZO NUMERICO nella configurazione della pagina, il log
 * del motore riporta:
 *
 *   STUN-CLIENT(srflx(IP4:<indirizzo-locale>:<porta>/UDP|
 *                     IP4:<stun-pubblico>:3478/UDP)):
 *       Received mapped address: IP4:<indirizzo-pubblico-vero>:<porta>/UDP
 *
 * cioe' l'indirizzo pubblico vero della macchina, restituito da un server che
 * qualcun altro gestisce, mentre la PAGINA vedeva l'uscita del proxy perche'
 * il candidato reale viene riscritto. Una fuga invisibile a qualunque
 * controllo lato pagina.
 *
 * Con un NOME invece del numero non succede, ma per un motivo che non e' una
 * difesa: il cancello DNS rifiuta la risoluzione, quindi il pacchetto non parte.
 * Ci proteggeva il formato dell'URL scelto dal rilevatore.
 *
 * NON si puo' usare la leva che c'era gia'. `GetForceProxy()`
 * (`media.peerconnection.ice.proxy_only`) rifiuta la CREAZIONE del socket UDP,
 * quindi cancella anche i candidati host: misurato su tre sistemi studiati,
 * zero candidati e il rilevatore che segnala la pagina come manomessa, cioe'
 * il peggiore dei suoi esiti.
 *
 * Qui si lascia creare il socket e si rifiuta l'INVIO verso destinazioni
 * pubbliche. Restano il multicast (l'mDNS che offusca gli host), il loopback e
 * le reti private, che non escono dalla macchina. Il risultato e' che un server
 * scritto come numero si comporta ESATTAMENTE come uno scritto come nome:
 * irraggiungibile. Che e' cio' che vede chiunque stia dietro un proxy.
 *
 * Torna true = RIFIUTA l'invio.
 */
inline bool StealthUscitaUdpDirettaVietata(const nr_transport_addr* to) {
  if (!mozilla::StaticPrefs::zoom_stealth_webrtc_no_direct_udp()) {
    return false;
  }
  if (!to) {
    return false;
  }
  if (to->ip_version == NR_IPV4) {
    uint32_t a = ntohl(to->u.addr4.sin_addr.s_addr);
    uint8_t o1 = (a >> 24) & 0xFF;
    uint8_t o2 = (a >> 16) & 0xFF;
    if (a == 0) return false;
    if (o1 == 127) return false;                          /* loopback */
    if (o1 == 10) return false;                           /* 10/8 */
    if (o1 == 172 && o2 >= 16 && o2 <= 31) return false;  /* 172.16/12 */
    if (o1 == 192 && o2 == 168) return false;             /* 192.168/16 */
    if (o1 == 169 && o2 == 254) return false;             /* link-local */
    if (o1 >= 224 && o1 <= 239) return false;             /* multicast, mDNS */
    return true;
  }
  if (to->ip_version == NR_IPV6) {
    const uint8_t* b = to->u.addr6.sin6_addr.s6_addr;
    if (b[0] == 0xFF) return false;                       /* multicast */
    if ((b[0] & 0xFE) == 0xFC) return false;              /* fc00::/7 */
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false; /* fe80::/10 */
    bool loopback = true;
    for (int k = 0; k < 15; ++k) {
      if (b[k] != 0) { loopback = false; break; }
    }
    if (loopback && b[15] == 1) return false;
    return true;
  }
  return false;
}

/* Same decision, plus the LOG_INFO line of the NrSocket::sendto path. Used by
 * that path only: the NrUdpSocketIpc::sendto path has never logged, and must
 * keep not logging. */
inline bool StealthUscitaUdpDirettaVietataLog(const nr_transport_addr* to) {
  if (!StealthUscitaUdpDirettaVietata(to)) {
    return false;
  }
  r_log(LOG_GENERIC, LOG_INFO,
        "stealth: UDP diretto rifiutato verso %s (dietro proxy)",
        to->as_string);
  return true;
}

#endif /* nr_stealth_proxy_udp_h__ */
