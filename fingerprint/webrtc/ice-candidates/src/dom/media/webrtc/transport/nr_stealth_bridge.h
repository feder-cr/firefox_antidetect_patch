/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* C-callable bridge from nICEr (C) into mozilla::StaticPrefs (C++).
 *
 * Stealthfox WebRTC realism: when running through a SOCKS5 proxy that
 * blocks UDP, real STUN binding requests time out. The hooks in
 * ice_candidate.cpp, ice_component.cpp, ice_ctx.cpp and addrs.cpp use these
 * helpers to read the proxy egress IP
 * (`zoom.stealth.webrtc.public_ip` or env var STEALTHFOX_WEBRTC_PUBLIC_IP)
 * and synthesize a STUN binding response so Firefox emits a srflx
 * candidate via its own existing callback path.
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
 * globale vero passi oltre un SOCKS che porta solo TCP.
 *
 * Chiamata da nr_stun_filter_addrs_for_ifname (stun/addrs.cpp), che scarta
 * l'host IPv6: stealth IPv6 suppression. The upstream
 * media.peerconnection.ice.disableIPv6 pref is dead in FF150, so we drop IPv6
 * there to stop the real global IPv6 host (and any IPv6 srflx derived from it)
 * leaking past a TCP-only SOCKS proxy. */
int nr_stealth_webrtc_disable_ipv6(void);

/* Torna 1 se, nel componente di `cand`, esiste gia' il srflx SINTETICO (un
 * altro candidato SERVER_REFLEXIVE con stun_server == NULL), cioe' se `cand`
 * (il srflx reale appena arrivato dallo STUN) non va annunciato; 0 altrimenti.
 * Il chiamante fa ABORT(R_NOT_FOUND) se torna 1: l'ABORT deve restare nella
 * funzione chiamante perche' salta alla sua etichetta `abort:`. `cand` e
 * `cand->component` non sono nulli. */
int nr_stealth_srflx_superseded(struct nr_ice_candidate_* cand);

/* Se STEALTHFOX_WEBRTC_PUBLIC_IP e' dichiarato e `addr` ha una porta > 0,
 * sostituisce l'IP di `addr` con quello dichiarato e tiene la porta. Altrimenti
 * non tocca `addr`. */
void nr_stealth_srflx_rewrite_ip(struct nr_transport_addr_* addr);

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

#ifdef __cplusplus
/* nICEr (src/ice) e' compilato come C++ e la funzione ha linkage C++, come
 * prima in ice_component.h: la dichiarazione sta FUORI dal blocco extern "C".
 * `nr_ice_component` e `nr_transport_addr` sono typedef delle due struct
 * dichiarate in cima (ice_ctx.h, transport_addr.h), quindi la firma e' la STESSA
 * di ice_component.h e le due dichiarazioni possono coesistere.
 *
 * Stealthfox: sintetizza UN srflx con l'IP di uscita dichiarato, quando nessun
 * srflx reale arrivera'. Torna 0 se l'ha iniettato, esce da sola se l'IP non e'
 * dichiarato (nessun proxy). Ha UN SOLO chiamante: il gathering, quando non e'
 * stato creato nessun candidato srflx. Il ramo di fallimento dello STUN in
 * ice_candidate.cpp non la chiama (imposta solo error_code e fa ABORT): il
 * vecchio commento in ice_component.h parlava di DUE chiamanti ed era falso.
 * Chiamarla altrove rimette il doppio srflx.
 *
 * Chiamata da nr_ice_component_initialize_udp (ice_component.cpp), dopo il ciclo
 * sui server STUN, una volta per indirizzo locale.
 *
 * Stealth fallback: inject one synthetic srflx con l'IP di uscita del
 * proxy SOLO se per questo indirizzo non e' stato creato nessun srflx
 * reale - il caso in cui la pagina usa URL TURN con parametri che
 * nICEr non sa interpretare, lasciando stun_servers/turn_servers a 0.
 * (La funzione esce comunque da sola se l'IP non e' dichiarato.)
 *
 * ⛔ PRIMA QUESTA CHIAMATA ERA INCONDIZIONATA, e il commento diceva che
 * quando anche il srflx reale riusciva "duplicates are harmless",
 * perche' lo swap post-STUN porta il reale allo stesso IP. Misurato il
 * 2026-08-25 contro il Firefox retail installato, sulla stessa
 * connessione: il retail emette 3 candidati (host:2, srflx:1), noi
 * dietro un proxy HTTP ne emettevamo 4 (host:2, **srflx:2**). Nessun
 * leak - l'IP vero non compariva - ma la FORMA non era quella di un
 * Firefox normale, ed e' la forma che ci deve rendere indistinguibili.
 *
 * L'iniezione resta INCONDIZIONATA - gattarla su `real_srflx_ct == 0`
 * e' stato provato e rimandava il ripiego a dopo il fallimento dello
 * STUN, cioe' secondi invece di ~200 ms: il sintetico non faceva piu'
 * in tempo, ed e' il caso per cui esiste. A garantire che il srflx sia
 * UNO SOLO ci pensa il lato opposto: quando lo STUN vero completa,
 * `ice_candidate.cpp` non annuncia il reale se il sintetico c'e' gia'.
 * Cosi' non c'e' ne' doppione ne' ritardo.
 * (NOTA: `real_srflx_ct` non esiste piu' nel chiamante, era una variabile
 * morta mai letta; il riferimento qui sopra e' storico.) */
int nr_ice_component_inject_fallback_srflx(struct nr_ice_component_* component,
                                           struct nr_transport_addr_* base_addr);
#endif

#endif /* nr_stealth_bridge_h__ */
