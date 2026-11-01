/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Multisampling on the Basic Render Driver (StealthGLMultisample.cpp). Mozilla
// drops it there for bug 978966, the Windows 8 device ignoring the blend
// function in multisampled draws. The Windows CI host has no GPU, so the draw
// below runs on the device every GPU-less Windows machine renders on.

#include "GLContext.h"
#include "GLContextProvider.h"
#include "MozFramebuffer.h"
#include "StealthGLMultisample.h"
#include "gfxPlatform.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"

namespace mozilla::gl {

constexpr char kForceEnabledPref[] = "webgl.force-enabled";

TEST(StealthGLMultisample, OnlyTheBasicRenderDriverWorkaroundIsUndone)
{
  auto restore =
      MakeScopeExit([] { Preferences::ClearUser(kForceEnabledPref); });
  Preferences::SetBool(kForceEnabledPref, true);
  EXPECT_TRUE(
      StealthKeepsMultisample(true, GLRenderer::MicrosoftBasicRenderDriver));
  // Never more than the driver reports.
  EXPECT_FALSE(
      StealthKeepsMultisample(false, GLRenderer::MicrosoftBasicRenderDriver));
  // Every other renderer keeps Mozilla's workarounds.
  EXPECT_FALSE(StealthKeepsMultisample(true, GLRenderer::Other));
  EXPECT_FALSE(StealthKeepsMultisample(true, GLRenderer::AdrenoTM320));
  // Without WebGL forced on, Mozilla's decision stands.
  Preferences::SetBool(kForceEnabledPref, false);
  EXPECT_FALSE(
      StealthKeepsMultisample(true, GLRenderer::MicrosoftBasicRenderDriver));
}

#ifdef XP_WIN

static GLuint CompileShader(GLContext* aGL, GLenum aType, const char* aSource) {
  const GLuint shader = aGL->fCreateShader(aType);
  aGL->fShaderSource(shader, 1, &aSource, nullptr);
  aGL->fCompileShader(shader);
  GLint ok = 0;
  aGL->fGetShaderiv(shader, LOCAL_GL_COMPILE_STATUS, &ok);
  EXPECT_TRUE(ok);
  return shader;
}

// Half-transparent red over opaque blue, 4x multisampled, resolved, read back.
// Correct blending gives half red, half blue in every channel. The bug of
// 978966 (the default blend function) would land the red unblended: 255,0,0.
TEST(StealthGLMultisample, BasicRenderDriverBlendsIntoAMultisampledTarget)
{
  gfxPlatform::GetPlatform();
  auto restore = MakeScopeExit([] {
    Preferences::ClearUser(kForceEnabledPref);
    Preferences::ClearUser("webgl.angle.force-warp");
  });
  // On a machine with a GPU, WARP is the Basic Render Driver too.
  Preferences::SetBool("webgl.angle.force-warp", true);
  Preferences::SetBool(kForceEnabledPref, true);

  nsCString failureId;
  const RefPtr<GLContext> gl = GLContextProviderEGL::CreateHeadless(
      {CreateContextFlags::PREFER_ES3}, &failureId);
  ASSERT_TRUE(gl)
  << failureId.get();
  ASSERT_TRUE(gl->MakeCurrent());
  ASSERT_EQ(gl->Renderer(), GLRenderer::MicrosoftBasicRenderDriver);
  // Before the hook: false here, and antialias:false for every page.
  ASSERT_TRUE(gl->IsSupported(GLFeature::framebuffer_multisample));
  ASSERT_GE(gl->MaxSamples(), 4u);

  const gfx::IntSize size(8, 8);
  const auto msaa = MozFramebuffer::Create(gl, size, 4, false, false);
  const auto resolved = MozFramebuffer::Create(gl, size, 0, false, false);
  ASSERT_TRUE(msaa);
  ASSERT_TRUE(resolved);

  // A triangle that covers the target, without vertex buffers.
  const GLuint vs = CompileShader(gl, LOCAL_GL_VERTEX_SHADER,
                                  "#version 300 es\n"
                                  "void main() {\n"
                                  "  vec2 p = vec2(float((gl_VertexID & 1) * 4) - 1.0,\n"
                                  "                float((gl_VertexID & 2) * 2) - 1.0);\n"
                                  "  gl_Position = vec4(p, 0.0, 1.0);\n"
                                  "}\n");
  const GLuint fs = CompileShader(gl, LOCAL_GL_FRAGMENT_SHADER,
                                  "#version 300 es\n"
                                  "precision mediump float;\n"
                                  "out vec4 color;\n"
                                  "void main() { color = vec4(1.0, 0.0, 0.0, 0.5); }\n");
  const GLuint program = gl->fCreateProgram();
  gl->fAttachShader(program, vs);
  gl->fAttachShader(program, fs);
  gl->fLinkProgram(program);
  GLint linked = 0;
  gl->fGetProgramiv(program, LOCAL_GL_LINK_STATUS, &linked);
  ASSERT_TRUE(linked);

  gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, msaa->mFB);
  gl->fViewport(0, 0, size.width, size.height);
  gl->fClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  gl->fClear(LOCAL_GL_COLOR_BUFFER_BIT);
  gl->fEnable(LOCAL_GL_BLEND);
  gl->fBlendFunc(LOCAL_GL_SRC_ALPHA, LOCAL_GL_ONE_MINUS_SRC_ALPHA);
  gl->fUseProgram(program);
  gl->fDrawArrays(LOCAL_GL_TRIANGLES, 0, 3);
  gl->fDisable(LOCAL_GL_BLEND);

  gl->fBindFramebuffer(LOCAL_GL_READ_FRAMEBUFFER, msaa->mFB);
  gl->fBindFramebuffer(LOCAL_GL_DRAW_FRAMEBUFFER, resolved->mFB);
  gl->fBlitFramebuffer(0, 0, size.width, size.height, 0, 0, size.width,
                       size.height, LOCAL_GL_COLOR_BUFFER_BIT, LOCAL_GL_NEAREST);
  gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, resolved->mFB);
  uint8_t pixel[4] = {};
  gl->fReadPixels(4, 4, 1, 1, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, pixel);
  EXPECT_NEAR(pixel[0], 128, 1);
  EXPECT_EQ(pixel[1], 0);
  EXPECT_NEAR(pixel[2], 127, 1);

  gl->fUseProgram(0);
  gl->fDeleteProgram(program);
  gl->fDeleteShader(vs);
  gl->fDeleteShader(fs);
}

#endif  // XP_WIN

}  // namespace mozilla::gl
