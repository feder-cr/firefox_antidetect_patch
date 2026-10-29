/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// fingerprint/webgl/shader-language: la resa di uno shader destinata alla
// PAGINA (getTranslatedShaderSource), nella lingua dichiarata da
// `zoom.stealth.webgl.shader_output_language`. Gancio in
// WebGLShader::CompileShader (WebGLShader.cpp); lettore in
// WebGLContext::GetCompileResult (WebGLContext.cpp). Also the reader of the
// declaration itself (StealthDeclaredShaderOutput) and the choice of the
// language to compile with (StealthShaderOutputLanguage), called from
// WebGLContext::CreateShaderValidator (WebGLShaderValidator.cpp).

#include <string>

#include "GLContext.h"
#include "StealthWebGLShaderLanguage.h"
#include "WebGLContext.h"
#include "WebGLShader.h"
#include "WebGLShaderValidator.h"
#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsDebug.h"
#include "nsString.h"

namespace mozilla {

namespace webgl {

// L'UNICO posto che legge la lingua dichiarata, e l'unico che sa tradurre il
// nome in una costante di ANGLE. Se questa conoscenza stesse in due punti, i
// due potrebbero rispondere diverso e nessuno se ne accorgerebbe: e' la stessa
// forma del difetto della geometria del mouse, dove la stessa aritmetica era
// ricopiata in tre posti.
//
// `Nothing()` vuol dire "nessuna dichiarazione", e allora la lingua resta
// quella che il contesto GL impone. Un nome dichiarato che non conosciamo e'
// anch'esso `Nothing()` E un avviso: inventare un default sarebbe il ripiego
// che la regola 7 vieta, e crashare qui spegnerebbe WebGL a un utente per un
// errore di battitura in una pref.
Maybe<ShShaderOutput> StealthDeclaredShaderOutput() {
  auto lock = StaticPrefs::zoom_stealth_webgl_shader_output_language();
  const nsCString declared(*lock);
  if (declared.IsEmpty()) {
    return Nothing();
  }
  if (declared.EqualsLiteral("essl")) {
    return Some(SH_ESSL_OUTPUT);
  }
  NS_WARNING(
      "zoom.stealth.webgl.shader_output_language dichiara una lingua che questa "
      "build non conosce; la traduzione resta quella del contesto GL");
  return Nothing();
}

ShShaderOutput StealthShaderOutputLanguage(gl::GLContext* gl,
                                           bool aPerLaPagina) {
  // Letta sempre, anche per il driver: l'avviso per un nome sconosciuto parte
  // a ogni costruzione del validatore.
  const auto dichiarata = StealthDeclaredShaderOutput();
  if (aPerLaPagina && dichiarata) {
    return *dichiarata;
  }
  return ShaderOutputPerIlDriver(gl);
}

}  // namespace webgl

// Quello che la PAGINA legge (WebGLContext::GetCompileResult), quando la
// lingua e' dichiarata e non coincide con quella del contesto GL. Il driver ha
// gia' ricevuto `mObjectCode` in `WebGLShader::CompileShader` e non lo si
// tocca: i pixel non cambiano. Vuoto vuol dire "niente da dichiarare", che e'
// il caso di ogni build Windows, dove il contesto passa da ANGLE ed e' gia'
// nella lingua giusta.
//
// La resa destinata alla PAGINA, nella lingua dichiarata. Vuota quando non
// c'e' niente da dichiarare o quando la lingua dichiarata e' gia' quella che
// il contesto usa - cioe' su Windows, dove il contesto passa da ANGLE ed e'
// gia' GLES: li' questo campo resta vuoto e non si paga niente.
//
// Non e' una seconda fonte di verita' per la stessa cosa: sono due
// consumatori diversi dello stesso albero validato, il driver e la pagina.
// Cio' che si dichiara e' la LINGUA, che ha dominio finito con un elemento
// solo; la stringa resta calcolata dal traduttore, perche' il suo dominio e'
// infinito e una tabella sarebbe vietata.
const std::string& WebGLShader::ObjectCodePerLaPagina() const {
  return mObjectCodePerLaPagina;
}

// Chiamata da WebGLShader::CompileShader con l'esito della validazione per il
// driver; il risultato sostituisce `mObjectCodePerLaPagina` (quindi un
// risultato vuoto azzera anche il valore di una compilazione precedente: dopo
// una compilazione fallita non resta nessuna stringa vecchia).
//
// Membro di WebGLShader e non funzione libera perche'
// `WebGLContext::CreateShaderValidator` e' protetta e WebGLContext dichiara
// amica solo `WebGLShader` (WebGLContext.h `friend class WebGLShader`).
std::string WebGLShader::StealthPerPageObjectCode(const bool aValid) {
  MOZ_ASSERT(aValid == mCompileResults->mValid);

  if (!aValid) {
    return {};
  }

  // La resa per la PAGINA, quando la lingua dichiarata non e' quella che il
  // contesto GL impone.
  //
  // Si compila per il driver la traduzione fatta in CompileShader, cosi' il
  // rendering non cambia di un pixel, e si produce una seconda resa nella
  // lingua dichiarata per chi legge `getTranslatedShaderSource`. Le due escono
  // dallo stesso traduttore ANGLE imbarcato con le stesse risorse dichiarate,
  // quindi la seconda e' identica a quella di una build Windows per
  // COSTRUZIONE, non per coincidenza misurata.
  //
  // Su Windows il contesto passa da ANGLE, che si presenta come GLES, quindi la
  // lingua dichiarata coincide con quella del contesto, il confronto qui sotto
  // e' falso e non si traduce due volte. Il costo lo paga solo la piattaforma
  // che ha il difetto, e solo per gli shader che vengono davvero compilati.
  //
  // ⛔ La stringa esposta non e' quella compilata. Nessuna pagina puo'
  // accorgersene - il driver non e' osservabile da li' - ma va scritto, perche'
  // e' vero.
  gl::GLContext* const gl = mContext->gl;
  const auto dichiarata = webgl::StealthDeclaredShaderOutput();
  if (!dichiarata || *dichiarata == webgl::ShaderOutputPerIlDriver(gl)) {
    return {};
  }

  const auto perLaPagina = mContext->CreateShaderValidator(mType, true);
  if (!perLaPagina) {
    return {};
  }

  const auto resa = perLaPagina->ValidateAndTranslate(mSource.c_str());
  // Se la seconda traduzione fallisce si tiene quella del driver: una
  // risposta nella lingua sbagliata e' un difetto di realness, una
  // risposta vuota e' una pagina rotta.
  if (resa && resa->mValid) {
    return resa->mObjectCode;
  }

  NS_WARNING(
      "la resa nella lingua dichiarata non ha validato; "
      "getTranslatedShaderSource restituira' quella del driver");
  return {};
}

}  // namespace mozilla
