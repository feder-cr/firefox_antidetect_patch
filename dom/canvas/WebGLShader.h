/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGL_SHADER_H_
#define WEBGL_SHADER_H_

#include <memory>
#include <string>
#include <vector>

#include "GLDefs.h"
#include "WebGLObjectModel.h"
#include "mozilla/MemoryReporting.h"

namespace mozilla {

namespace webgl {
class ShaderValidatorResults;
}  // namespace webgl

class WebGLShader final : public WebGLContextBoundObject {
  friend class WebGLContext;
  friend class WebGLProgram;

  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(WebGLShader, override)

 public:
  WebGLShader(WebGLContext* webgl, GLenum type);

 protected:
  ~WebGLShader() override;

 public:
  // GL funcs
  void CompileShader();
  void ShaderSource(const std::string& source);

  // Util funcs
  size_t CalcNumSamplerUniforms() const;
  size_t NumAttributes() const;

  const auto& CompileResults() const { return mCompileResults; }
  const auto& CompileLog() const { return mCompilationLog; }
  bool IsCompiled() const { return mCompilationSuccessful; }

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
  const std::string& ObjectCodePerLaPagina() const {
    return mObjectCodePerLaPagina;
  }

 private:
  void BindAttribLocation(GLuint prog, const std::string& userName,
                          GLuint index) const;
  void MapTransformFeedbackVaryings(
      const std::vector<std::string>& varyings,
      std::vector<std::string>* out_mappedVaryings) const;

 public:
  // Other funcs
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 public:
  const GLuint mGLName;
  const GLenum mType;

 protected:
  std::string mSource;

  std::unique_ptr<const webgl::ShaderValidatorResults>
      mCompileResults;  // Never null.
  bool mCompilationSuccessful = false;
  std::string mCompilationLog;
  std::string mObjectCodePerLaPagina;
};

}  // namespace mozilla

#endif  // WEBGL_SHADER_H_
