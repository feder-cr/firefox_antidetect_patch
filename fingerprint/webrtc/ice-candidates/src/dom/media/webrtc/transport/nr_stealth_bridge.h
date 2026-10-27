/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* C-callable bridge from nICEr (C) into mozilla::StaticPrefs (C++).
 *
 * Stealthfox WebRTC realism: when running through a SOCKS5 proxy that
 * blocks UDP, real STUN binding requests time out. The hook in
 * stun_client_ctx.cpp uses these helpers to read the proxy egress IP
 * (`zoom.stealth.webrtc.public_ip` or env var STEALTHFOX_WEBRTC_PUBLIC_IP)
 * and synthesize a STUN binding response so Firefox emits a srflx
 * candidate via its own existing callback path.
 */

#ifndef nr_stealth_bridge_h__
#define nr_stealth_bridge_h__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UNA SOLA FONTE: la variabile d'ambiente, dichiarata da `invisible_core`.
 * Il ramo che leggeva anche la pref `zoom.stealth.webrtc.*` e' stato tolto il
 * 2026-08-25 - il perche' per esteso sta in cima al .cpp. Questa intestazione
 * descriveva una precedenza (env prima, pref dopo) che il codice NON aveva:
 * era gia' una terza versione della stessa cosa. */

/* Copia il valore di STEALTHFOX_WEBRTC_PUBLIC_IP in `buf` (terminato da NUL).
 * Torna i byte scritti escluso il NUL, 0 se la variabile e' assente o vuota.
 * `buf_len` deve essere >= 1. Zero significa "non dichiarato": il chiamante
 * non emette il candidato sintetico, e nessun valore viene inventato. */
size_t nr_stealth_get_webrtc_public_ip(char* buf, size_t buf_len);

/* Torna 1 se i candidati host IPv6 vanno tolti dall'ICE gathering, 0 altrimenti.
 * Legge STEALTHFOX_WEBRTC_DISABLE_IPV6: "0" spegne esplicitamente, qualunque
 * altro valore non vuoto accende, assente = spento. La pref upstream
 * media.peerconnection.ice.disableIPv6 e' morta in FF150 (non la legge nessuno
 * nel percorso ICE), quindi questa e' la strada per impedire che l'IPv6 host
 * globale vero passi oltre un SOCKS che porta solo TCP. */
int nr_stealth_webrtc_disable_ipv6(void);

#ifdef __cplusplus
}
#endif

#endif /* nr_stealth_bridge_h__ */
