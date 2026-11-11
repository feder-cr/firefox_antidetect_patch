/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* C-callable bridge from nICEr (C) into the engine's own code. The hooks in
 * ice_ctx.cpp and addrs.cpp use these helpers to choose the addresses of the
 * gather. (The srflx is no longer declared or rewritten: it is the one a real
 * allocation gives, through the proxy the UDP of the PeerConnection takes.)
 */

#ifndef nr_stealth_bridge_h__
#define nr_stealth_bridge_h__

#include <stddef.h>

/* Forward declarations of the nICEr struct tags: the typedefs nr_ice_candidate,
 * nr_ice_component, nr_transport_addr and nr_local_addr name these same tags
 * (ice_ctx.h, transport_addr.h, local_addr.h), so the signatures below are the
 * ones the .cpp sees. struct nr_ice_candidate_ derives from a base struct, so
 * the functions that take it are C++ in practice: every includer is a .cpp. */
struct nr_ice_candidate_;
struct nr_ice_component_;
struct nr_transport_addr_;
struct nr_local_addr_;

#ifdef __cplusplus
extern "C" {
#endif

/* Torna 1 se i candidati host IPv6 vanno tolti dall'ICE gathering, 0 altrimenti.
 * Legge STEALTHFOX_WEBRTC_DISABLE_IPV6: "0" spegne esplicitamente, qualunque
 * altro valore non vuoto accende, assente = spento. La pref upstream
 * media.peerconnection.ice.disableIPv6 e' morta in FF150 (non la legge nessuno
 * nel percorso ICE), quindi questa e' la strada per impedire che l'IPv6 host
 * globale vero passi oltre un SOCKS che porta solo TCP.
 *
 * Chiamata da nr_stun_filter_addrs_for_ifname (stun/addrs.cpp), che scarta
 * l'host IPv6: stealth IPv6 suppression. The upstream
 * media.peerconnection.ice.disableIPv6 pref is dead in FF150, so we drop IPv6
 * there to stop the real global IPv6 host (and any IPv6 srflx derived from it)
 * leaking past a TCP-only SOCKS proxy. */
int nr_stealth_webrtc_disable_ipv6(void);

/* Torna 1 se l'INDIRIZZO di `target` e' il wildcard (0.0.0.0 o ::), 0 altrimenti;
 * la porta non conta. Il chiamante (nr_ice_get_default_address) in tal caso
 * rinuncia alla sonda della rotta predefinita. Se torna 1 ha gia' scritto la
 * riga di log. `target` non puo' essere NULL. */
int nr_stealth_refuse_wildcard_target(const struct nr_transport_addr_* target);

/* Sceglie gli indirizzi per l'ICE gathering dopo la sonda della rotta
 * predefinita. Torna 0 e imposta *addrs (e *addr_ct se usa quelli di default),
 * oppure R_FAILED e non tocca nulla. Se default_addr_ct > 0 usa default_addrs;
 * se la sonda non ha dato nulla ma *addr_ct (gli indirizzi locali filtrati) e'
 * > 0 ripiega su local_addrs, con *addr_ct invariato; se sono entrambi zero
 * scrive l'errore "failed to find default addresses" e torna R_FAILED. */
int nr_stealth_ice_pick_addrs(const char* label,
                              struct nr_local_addr_* local_addrs,
                              struct nr_local_addr_* default_addrs,
                              int default_addr_ct,
                              struct nr_local_addr_** addrs, int* addr_ct);

#ifdef __cplusplus
}
#endif

#endif /* nr_stealth_bridge_h__ */
