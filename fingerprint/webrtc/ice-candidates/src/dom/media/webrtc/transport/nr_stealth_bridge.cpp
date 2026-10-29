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

extern "C" size_t nr_stealth_get_webrtc_public_ip(char* buf, size_t buf_len) {
  return CopyEnvTo("STEALTHFOX_WEBRTC_PUBLIC_IP", buf, buf_len);
}

extern "C" int nr_stealth_webrtc_disable_ipv6(void) {
  /* "0" spegne esplicitamente; qualunque altro valore non vuoto accende.
   * Assente = spento: senza dichiarazione non si tocca l'ICE gathering. */
  const char* env = getenv("STEALTHFOX_WEBRTC_DISABLE_IPV6");
  if (env && env[0] != '\0') {
    return (env[0] != '0') ? 1 : 0;
  }
  return 0;
}

/* Stealthfox: UN SOLO srflx, sempre.
 *
 * Se nel componente c'e' gia' il srflx SINTETICO, questo reale non si
 * annuncia. Misurato il 2026-08-25 contro il retail installato: il
 * riferimento emette un solo srflx per famiglia, noi dietro un proxy
 * ne emettevamo DUE - nessun leak (lo swap qui sotto porta il reale
 * allo stesso IP) ma una forma che un Firefox normale non produce.
 *
 * Si sopprime il REALE e non il sintetico perche' il sintetico arriva
 * a ~200 ms mentre questo arriva dopo il round-trip STUN: rinunciare a
 * quello gia' annunciato significherebbe o un buco o un ritardo di
 * secondi nel caso in cui lo STUN poi fallisce, che e' proprio il caso
 * per cui il ripiego esiste. Funzionalmente non si perde nulla: lo
 * swap qui sotto riscrive comunque questo indirizzo con l'IP del
 * proxy, che non instrada.
 *
 * Il discriminante e' `stun_server`: il sintetico lo mette a NULL, un
 * srflx vero ne ha sempre uno. */
extern "C" int nr_stealth_srflx_superseded(nr_ice_candidate* cand) {
  MOZ_ASSERT(cand);
  MOZ_ASSERT(cand->component);
  nr_ice_candidate* altro;
  TAILQ_FOREACH(altro, &cand->component->candidates, entry_comp) {
    if (altro != cand && altro->type == SERVER_REFLEXIVE &&
        altro->stun_server == NULL) {
      return 1;
    }
  }
  return 0;
}

/* Stealthfox WebRTC realism: when proxy egress IP is configured,
 * replace the real public IP returned by STUN with the proxy IP so
 * srflx.address matches the HTTP source IP (a leak check compares
 * the first frontend IP with the public one). The mapped port from
 * real NAT mapping is preserved — we only swap the IP. */
extern "C" void nr_stealth_srflx_rewrite_ip(nr_transport_addr* addr) {
  MOZ_ASSERT(addr);
  char stealth_ip[64];
  if (nr_stealth_get_webrtc_public_ip(stealth_ip, sizeof(stealth_ip)) > 0) {
    uint16_t mapped_port = 0;
    if (nr_transport_addr_get_port(addr, &mapped_port) == 0 &&
        mapped_port > 0) {
      nr_str_port_to_transport_addr(stealth_ip, mapped_port, IPPROTO_UDP, addr);
    }
  }
}

/* Async timer callback: transition synthetic srflx from INITIALIZING to
 * INITIALIZED and notify nICEr's gather state machine. */
static void nr_ice_stealth_srflx_fire_ready(NR_SOCKET /* s */, int /* how */,
                                            void *cb_arg)
{
    nr_ice_candidate *cand = (nr_ice_candidate *)cb_arg;
    cand->ready_cb_timer = 0;

    /* La porta esterna si decide ADESSO, non all'iniezione: un NAT vero mappa
     * porte interne diverse su porte esterne diverse, e la porta interna (la
     * base) esiste solo dopo il bind del socket - all'iniezione vale 0.
     *
     * Misurato sul Firefox retail installato il 2026-08-25: `host 59463 ->
     * srflx 7884` sull'IPv4 (rimappato dal NAT) e `host 59464 -> srflx 59464`
     * sull'IPv6 (nessun NAT, porta identica). In entrambi i casi la porta
     * esterna e' funzione della base.
     *
     * Senza questo, il seme era il solo IP: costante per tutta la sessione,
     * quindi OGNI componente e OGNI m-line ricevevano la STESSA porta. In un
     * SDP reale si leggeva `host comp1=51236 comp2=51237` (diverse) accanto a
     * `srflx comp1=55795 comp2=55795` (identiche) - una firma che nessun NAT
     * produce, visibile a chiunque apra l'SDP. */
    {
        nr_ice_candidate *hc;
        uint16_t porta_base = 0, porta_mia = 0;
        /* ⛔ IL PROPRIO host, non il PRIMO della lista. L'iniezione avviene una
         * volta per indirizzo locale, quindi su una macchina con DUE interfacce
         * nascono DUE srflx sintetici; prendendo il primo host entrambi
         * derivavano dalla stessa base e ricevevano la STESSA porta. E
         * l'indirizzo coincide gia' per costruzione - e' l'uscita dichiarata,
         * una sola - quindi il risultato erano due righe `typ srflx` identiche
         * in tutto.
         *
         * Misurato il 2026-08-25 a occhio, headful, dietro un socks5 reale:
         * `82.40.95.144 54976` DUE VOLTE nello stesso SDP. Nessun NAT mappa due
         * socket locali distinti sulla stessa porta esterna. [B180]
         *
         * ⛔ Nessun banco l'aveva colto, e la ragione va ricordata: giravano
         * headless con UNA sola interfaccia utilizzabile, dove l'iniezione
         * avviene una volta e il difetto non esiste. Il numero di interfacce
         * non era una variabile controllata da nessuna misura.
         *
         * `cand->base` porta gia' l'indirizzo dell'interfaccia da cui QUESTO
         * srflx e' nato (copiato da `base_addr` all'iniezione). La sua porta
         * li' vale 0, ma l'indirizzo basta a riconoscere il proprio host, che a
         * questo punto e' legato e una porta ce l'ha. Il confronto usa
         * MODE_ADDR apposta: ignora la porta, che e' cio' che stiamo cercando. */
        TAILQ_FOREACH(hc, &cand->component->candidates, entry_comp) {
            if (hc->type == HOST && hc->local_protocol == IPPROTO_UDP &&
                hc->component_id == cand->component_id &&
                !nr_transport_addr_cmp(&hc->addr, &cand->base,
                                       NR_TRANSPORT_ADDR_CMP_MODE_ADDR) &&
                nr_transport_addr_get_port(&hc->addr, &porta_base) == 0 &&
                porta_base > 0) {
                break;
            }
            porta_base = 0;
        }
        if (porta_base == 0) {
            /* Nessun host corrisponde alla nostra base. Succede sul ramo senza
             * indirizzi locali, dove la base e' quella sintetica `192.168.1.1`.
             * Si ricade sul PRIMO host - il comportamento di prima, peggiore ma
             * non rotto - invece di lasciare la porta PROVVISORIA, che e'
             * funzione del solo IP e quindi identica per ogni candidato. */
            TAILQ_FOREACH(hc, &cand->component->candidates, entry_comp) {
                if (hc->type == HOST && hc->local_protocol == IPPROTO_UDP &&
                    hc->component_id == cand->component_id &&
                    nr_transport_addr_get_port(&hc->addr, &porta_base) == 0 &&
                    porta_base > 0) {
                    break;
                }
                porta_base = 0;
            }
        }
        if (porta_base > 0 &&
            nr_transport_addr_get_port(&cand->addr, &porta_mia) == 0) {
            unsigned int seme = (unsigned int)porta_mia
                              + (unsigned int)porta_base * 2654435761u;
            UINT2 nuova = (UINT2)(49152 + (seme % 16384));
            if (nr_transport_addr_set_port(&cand->addr, nuova) == 0)
                nr_transport_addr_fmt_addr_string(&cand->addr);
        }
    }

    cand->state = NR_ICE_CAND_STATE_INITIALIZED;
    nr_ice_gather_finished_cb(0, 0, cand);
}

/* Stealth fallback: when the page configured iceServers but zero srflx
 * candidates ended up created (DNS resolution failed for the STUN server,
 * UDP filtered, etc.), inject one synthetic srflx with the proxy egress
 * IP so the page sees a coherent "user behind NAT with STUN-able net"
 * profile (a check that compares the first frontend IP with the public one).
 *
 * Returns 0 if a candidate was injected, non-zero otherwise. */
int nr_ice_component_inject_fallback_srflx(nr_ice_component *component,
                                           nr_transport_addr *base_addr)
{
    int r, _status;
    char public_ip_buf[64];
    nr_ice_candidate *cand = 0;
    char label[128];
    UINT2 srflx_port;
    char foundation[16];
    unsigned int o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    unsigned int port_seed;

    if (nr_stealth_get_webrtc_public_ip(public_ip_buf, sizeof(public_ip_buf)) == 0)
        return R_NOT_FOUND;

    /* ⛔ SOLO SE LA PAGINA HA CONFIGURATO ALMENO UN SERVER. Un candidato
     * server-reflexive nasce dalla XOR-MAPPED-ADDRESS di una risposta STUN o
     * TURN: senza nessuno a cui chiedere non puo' esistere, in nessun browser.
     *
     * MISURATO il 2026-08-25, dietro proxy, tre configurazioni nella stessa
     * sessione: `new RTCPeerConnection()` senza argomenti, `{iceServers: []}`,
     * e uno STUN vero. Tutte e tre producevano `host 2, srflx 1`. Le prime due
     * sono impossibili per un Firefox reale.
     *
     * ⛔ E COSTA TRE RIGHE DI JAVASCRIPT VEDERLO. Niente rete, niente server,
     * niente correlazione: si crea una connessione senza server e si guarda se
     * arriva un srflx. E' molto piu' economico del banco che correla lato
     * server (le nostre note di ricerca), e non lascia scampo.
     *
     * Non e' il gate su `real_srflx_ct` che era stato provato e tolto: quello
     * ASPETTAVA il fallimento dello STUN vero, quindi rimandava il ripiego di
     * secondi e il sintetico non faceva piu' in tempo. Questo non aspetta
     * niente - i contatori sono noti prima di iniziare - e non introduce
     * nessun ritardo.
     *
     * Con zero server configurati non emettiamo piu' nessun srflx, che e'
     * esattamente cio' che fa un Firefox qualunque nella stessa situazione. */
    if (component->stream->stun_server_ct == 0 &&
        component->stream->turn_server_ct == 0)
        return R_NOT_FOUND;

    /* ⛔ UNO SOLO PER COMPONENTE. La chiamata a questa funzione sta dentro il
     * ciclo sugli indirizzi locali, quindi su una macchina con DUE interfacce
     * veniva eseguita due volte e nascevano DUE srflx sintetici: stesso
     * indirizzo per costruzione (l'uscita dichiarata e' una sola), stessa
     * priorita', e - finche' non e' stato corretto - anche la stessa porta.
     *
     * MISURATO il 2026-08-25 contro il retail 151.0.4 FIRMATO, lanciato senza
     * automazione su una pagina locale che si legge da sola, tre corse
     * interleaved contro tre nostre: **con due interfacce il retail produce UN
     * SOLO srflx**, 3 volte su 3, con priorita' `1685987327` - cioe' esattamente
     * il valore che calcoliamo gia'. Non due con porte diverse: uno.
     *
     * Per COMPONENTE e non per sessione: un SDP con audio e video ha piu'
     * componenti, e li' un browser vero ne mostra uno ciascuno con porte
     * diverse fra loro. E' proprio il caso che la derivazione della porta dalla
     * base esiste per servire.
     *
     * Il nostro si riconosce dal fatto che non ha `stun_server`: e' lo stesso
     * discriminante che `ice_candidate.cpp` usa per sopprimere il srflx REALE
     * quando il sintetico c'e' gia'.
     *
     * ⛔ La voce di [B180] diceva "il rimedio NON e' iniettarne uno solo,
     * perche' un solo srflx con due interfacce e' anch'esso un profilo raro".
     * Era un'affermazione SENZA MISURA, scritta con la stessa sicurezza dei
     * fatti misurati accanto, e il confronto col retail la smentisce. */
    {
        nr_ice_candidate *esistente;
        TAILQ_FOREACH(esistente, &component->candidates, entry_comp) {
            if (esistente->type == SERVER_REFLEXIVE && !esistente->stun_server)
                return R_ALREADY;
        }
    }

    /* La porta si deriva dall'IP **e dalla PORTA DELLA BASE**, non dal solo IP.
     *
     * Un NAT vero mappa porte interne diverse su porte esterne diverse.
     * Misurato il 2026-08-25 sul Firefox retail installato: `host 54402 ->
     * srflx 1798` sull'IPv4 (rimappato) e `host 54403 -> srflx 54403`
     * sull'IPv6 (nessun NAT, porta identica). In entrambi i casi la porta
     * esterna e' funzione della base.
     *
     * Col solo IP il seme era costante per l'intera sessione, quindi OGNI
     * componente e OGNI m-line ricevevano la STESSA porta: in un SDP reale si
     * vedeva `host comp1=51236 comp2=51237` (diverse) e
     * `srflx comp1=55795 comp2=55795` (identiche). Nessun NAT si comporta
     * cosi', ed e' visibile a chiunque legga l'SDP.
     *
     * Il commento che stava qui diceva che la porta della base non e'
     * disponibile, e per `base_addr` e' vero - arriva dall'indirizzo
     * dell'interfaccia, prima del bind. Ma il candidato HOST di questo
     * componente e' gia' nella lista e la sua porta e' assegnata: e' quella la
     * base di cui il nostro srflx dice di essere il riflesso. */
    sscanf(public_ip_buf, "%u.%u.%u.%u", &o1, &o2, &o3, &o4);
    port_seed = o1 * 7 + o2 * 13 + o3 * 19 + o4 * 31;
    srflx_port = (UINT2)(49152 + (port_seed % 16384));
    /* Porta PROVVISORIA: qui la base non e' ancora legata (il commento
     * originale su base_addr->port == 0 e' corretto, verificato provando a
     * leggerla), quindi la porta definitiva si ricalcola nel timer, dove la
     * base ha un numero vero. Vedi nr_ice_stealth_srflx_fire_ready. */

    if (!(cand = (nr_ice_candidate *)calloc(1, sizeof(nr_ice_candidate))))
        ABORT(R_NO_MEMORY);

    cand->ctx          = component->stream->ctx;
    cand->isock        = NULL;
    cand->osock        = NULL;
    cand->type         = SERVER_REFLEXIVE;
    cand->tcp_type     = (nr_socket_tcp_type)0;
    cand->stun_server  = NULL;
    cand->component_id = component->component_id;
    cand->component    = component;
    cand->stream       = component->stream;

    if ((r = nr_str_port_to_transport_addr(public_ip_buf, srflx_port, IPPROTO_UDP,
                                           &cand->addr)))
        ABORT(r);
    if (base_addr) {
        if ((r = nr_transport_addr_copy(&cand->base, base_addr)))
            ABORT(r);
    } else {
        /* No real local address available (Fission content process before
         * mStunAddrs IPC arrives). Synthesize a fake LAN base so the
         * candidate has a coherent raddr/rport. */
        if ((r = nr_str_port_to_transport_addr("192.168.1.1", srflx_port, IPPROTO_UDP,
                                               &cand->base)))
            ABORT(r);
    }
    cand->local_protocol = cand->base.protocol;

    /* Foundation = the index into ctx->foundations, exactly as
     * nr_ice_get_foundation assigns it (keyed by base addr + type + stun-server
     * addr). A real srflx shares the host's base but differs in type, so it
     * never matches a host entry and always takes a FRESH index — vanilla shows
     * the srflx foundation distinct from every host one. We can't call the
     * static nr_ice_get_foundation (it derefs cand->stun_server, NULL here), so
     * replicate its "append new foundation" branch: count existing entries, use
     * that as our index, and append our own so later candidates keep getting
     * correct, collision-free indices. (max+1 over the component list was wrong:
     * it collided with the host TCP foundation.) */
    {
        nr_ice_ctx *fctx = cand->ctx;
        nr_ice_foundation *f;
        int idx = 0, found = 0;
        /* REUSE the index of an existing SERVER_REFLEXIVE foundation on the
         * same base, exactly as nr_ice_get_foundation does (matching on
         * ip_version+protocol+IP, not port). The real-but-failed srflx
         * candidates created just above (one per STUN server) already
         * registered such a foundation for this base, so we inherit its small
         * index (~1, like vanilla) and stay STABLE across every gather round.
         * Only append a fresh one if none exists (e.g. TURN-only configs where
         * no real srflx was created). Blindly appending every round made the
         * foundation drift (4->6->2), itself a tell. */
        STAILQ_FOREACH(f, &fctx->foundations, entry) {
            if (f->type == (int)SERVER_REFLEXIVE &&
                !nr_transport_addr_cmp(&cand->base, &f->addr,
                                       NR_TRANSPORT_ADDR_CMP_MODE_ADDR)) {
                found = 1;
                break;
            }
            idx++;
        }
        if (!found && (f = R_NEW(nr_ice_foundation))) {
            f->index = idx;
            nr_transport_addr_copy(&f->addr, &cand->base);
            f->type = SERVER_REFLEXIVE;
            STAILQ_INSERT_TAIL(&fctx->foundations, f, entry);
        }
        snprintf(foundation, sizeof(foundation), "%d", idx);
    }

    snprintf(label, sizeof(label), "srflx-stealth(%s)", public_ip_buf);
    if (!(cand->label = strdup(label)))
        ABORT(R_NO_MEMORY);
    if (!(cand->foundation = strdup(foundation)))
        ABORT(R_NO_MEMORY);

    /* Priority must reconstruct nr_ice_candidate_compute_priority for a
     * SERVER_REFLEXIVE/UDP candidate:
     *   (type_pref<<24)|(iface_pref<<16)|(dir<<13)|(stun_priority<<8)|(256-comp)
     * type_pref=100 (SRV_RFLX), dir=0 (UDP), stun_priority=31 (=31-stun id 0).
     * interface_preference is interface-specific (~126/127) and MUST match the
     * real host candidate on this interface; the old hardcoded 0xFFFF gave
     * local_pref=65535, a value no genuine nICEr candidate ever has (real range
     * ~32256-32704) — an instant tell. Read it from the UDP host candidate
     * already created for this address (bits 16-23 of its priority). */
    {
        UCHAR iface_pref = 126;  /* fallback if no host candidate is present yet */
        nr_ice_candidate *hc;
        /* ⛔ QUI C'ERA UNA CORREZIONE, ED E' STATA TOLTA DOPO IL CONFRONTO COL
         * RETAIL. Cercava l'host della PROPRIA interfaccia invece del primo,
         * per dare priorita' diverse ai due srflx sintetici. Non serviva, e la
         * ragione e' che i due srflx non dovrebbero esistere:
         *
         * Misurato il 2026-08-25 contro il retail 151.0.4 FIRMATO, lanciato
         * senza automazione su una pagina locale, tre corse interleaved: con
         * DUE interfacce il retail produce **UN SOLO srflx**, sempre, e la sua
         * priorita' e' `1685987327` - cioe' esattamente il valore che
         * calcoliamo gia'. Il valore era giusto; era il CONTEGGIO a essere
         * sbagliato, e due candidati che condividono una priorita' corretta
         * sono un problema solo perche' sono due.
         *
         * Togliere una correzione che non sposta la misura giusta e' meglio
         * che tenerla: e' codice che diverge da upstream a ogni rebase per
         * niente. Il conteggio si corregge dove nasce, cioe' nell'iniezione.
         * Vedi `72-next-steps.md`. */
        TAILQ_FOREACH(hc, &component->candidates, entry_comp) {
            if (hc->type == HOST && hc->local_protocol == IPPROTO_UDP && hc->priority) {
                iface_pref = (UCHAR)((hc->priority >> 16) & 0xFF);
                break;
            }
        }
        cand->priority = ((UINT4)100 << 24) | ((UINT4)iface_pref << 16) |
                         ((UINT4)0 << 13) | ((UINT4)31 << 8) |
                         (UINT4)(256 - cand->component_id);
    }

    nr_ice_candidate_compute_codeword(cand);
    cand->state = NR_ICE_CAND_STATE_INITIALIZING;

    TAILQ_INSERT_TAIL(&component->candidates, cand, entry_comp);
    component->candidate_ct++;
    cand->ctx->uninitialized_candidates++;

    r_log(LOG_ICE, LOG_INFO,
          "ICE-STEALTH(%s)/CAND(%s): injected fallback srflx %s (base=%s)",
          component->stream->label, cand->codeword,
          cand->addr.as_string, cand->base.as_string);

    /* Fire ~200 ms out, not immediately: a real srflx only arrives AFTER the
     * STUN round-trip, so host candidates are emitted first and the srflx
     * trickles in later (~RTT). Emitting it at 0 ms made it race/precede the
     * host candidates, which is unnatural and broke first-candidate-based
     * collectors (public test pages). 200 ms is a plausible STUN RTT and well
     * inside typical 3 s gathering windows. */
    NR_ASYNC_TIMER_SET(200, nr_ice_stealth_srflx_fire_ready, (void *)cand,
                       &cand->ready_cb_timer);
    cand = 0;

    _status = 0;
abort:
    if (_status && cand) {
        free(cand->label);
        free(cand->foundation);
        free(cand);
    }
    return _status;
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
