/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nr_stealth_bridge.h"

#include <cstdlib>
#include <cstring>

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
