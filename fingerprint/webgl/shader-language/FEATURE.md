# fingerprint/webgl/shader-language

Category: spoof
Prefs: zoom.stealth.webgl.shader_output_language
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/canvas/WebGLContext.cpp` (WebGLContext::GetCompileResult (translatedSource), already-hook): getTranslatedShaderSource returns the page-language translation when a declared language differs from the context's, and the driver output otherwise (empty string means nothing declared, which is every Windows build). The added comment block is in Italian, while the rest of the tree is English; flag for the maintainer.
- `dom/canvas/WebGLContext.h` (WebGLContext::CreateShaderValidator (declaration), already-hook): Adds a defaulted bool flag to choose the page-facing output language. The comment block is in Italian (same flag as in WebGLContext.cpp).
- `dom/canvas/WebGLShader.cpp` (WebGLShader::CompileShader, movable): No pref is read in this file's lines; zoom.stealth.webgl.shader_output_language is read indirectly through webgl::StealthDeclaredShaderOutput (WebGLShaderValidator.cpp). When the declared language differs from the driver's, the shader is translated a second time through ANGLE on every compile, so non-Windows builds with the pref set pay for an extra translation per shader. A failed second translat
- `dom/canvas/WebGLShader.h` (class WebGLShader (ObjectCodePerLaPagina accessor, mObjectCodePerLaPagina member), already-hook): Adds one std::string member per shader, empty on Windows. Comments are in Italian.
- `dom/canvas/WebGLShaderValidator.cpp` (webgl::StealthDeclaredShaderOutput (new, inside namespace webgl), webgl::ShaderOutputPerIlDriver (renamed from static ShaderOutput), WebGLContext::CreateShaderValidator, movable): ShaderOutput lost its static linkage and was renamed; grep of dom/canvas finds no other caller of the old name (only WebGLShader.cpp:132 and this file call the new ones), so the rename is complete. The function also moved from namespace mozilla into mozilla::webgl (the closing brace of namespace webgl moved below it). The pref is read on every CreateShaderValidator call, with no caching. An unknow
- `dom/canvas/WebGLShaderValidator.h` (namespace mozilla::webgl (ShaderOutputPerIlDriver and StealthDeclaredShaderOutput declarations), already-hook): Declarations only, plus a forward declaration of gl::GLContext and an include of mozilla/Maybe.h. Comments are in Italian.

## dom/canvas/WebGLContext.h: WebGLContext::CreateShaderValidator (aPerLaPagina)

`aPerLaPagina` chiede il validatore che produce la resa DESTINATA ALLA PAGINA, nella lingua dichiarata, invece di quella destinata al driver. Tutto il resto - risorse, opzioni, spec - e' identico per costruzione, perche' e' lo stesso corpo di funzione: se fossero due funzioni divergerebbero al primo limite che qualcuno aggiunge a una sola delle due.
