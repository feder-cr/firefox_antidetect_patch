/* The GL Firefox's WebGL uses on Linux, proven by using it before it is kept
 * (scripts/ci/lavapipe_build.py): the libraries of DIR alone, EGL's surfaceless
 * platform (no X11, no Wayland, no render node), a GLES 3 context as
 * GLContextProviderEGL::CreateHeadless asks, a clear read back, and a process
 * that has mapped no GL library but these.
 *
 *     gl_proof DIR
 *
 * One line per use, "-> OK" when it worked. */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

static void *egl;

static void *sym(const char *name) {
  void *f = dlsym(egl, name);
  if (!f) {
    PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
    f = gpa ? (void *)gpa(name) : NULL;
  }
  return f;
}

#define F(type, name) type name = (type)sym(#name)

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: gl_proof DIR\n");
    return 2;
  }
  char path[4096];
  snprintf(path, sizeof path, "%s/libEGL.so.1", argv[1]);
  egl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!egl) {
    printf("A libEGL.so.1 loads from its directory: %s -> FAIL\n", dlerror());
    return 1;
  }
  printf("A libEGL.so.1 loads from its directory -> OK\n");

  F(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay);
  F(PFNEGLINITIALIZEPROC, eglInitialize);
  F(PFNEGLBINDAPIPROC, eglBindAPI);
  F(PFNEGLCHOOSECONFIGPROC, eglChooseConfig);
  F(PFNEGLCREATECONTEXTPROC, eglCreateContext);
  F(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface);
  F(PFNEGLMAKECURRENTPROC, eglMakeCurrent);
  EGLDisplay dpy = eglGetPlatformDisplay
                       ? eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                       : EGL_NO_DISPLAY;
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API)) {
    printf("B a surfaceless display -> FAIL\n");
    return 1;
  }
  const EGLint want[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  const EGLint pbAttribs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLSurface pb = EGL_NO_SURFACE;
  if (eglChooseConfig(dpy, want, &config, 1, &count) && count == 1) {
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxAttribs);
    pb = eglCreatePbufferSurface(dpy, config, pbAttribs);
  }
  if (ctx == EGL_NO_CONTEXT || pb == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pb, pb, ctx)) {
    printf("B a surfaceless display and a GLES 3 context -> FAIL\n");
    return 1;
  }
  printf("B a surfaceless display (EGL %d.%d) and a GLES 3 context -> OK\n", major, minor);

  F(PFNGLGETSTRINGPROC, glGetString);
  F(PFNGLCLEARCOLORPROC, glClearColor);
  F(PFNGLCLEARPROC, glClear);
  F(PFNGLREADPIXELSPROC, glReadPixels);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  unsigned char px[4] = {0};
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  const int ok = renderer && strstr(renderer, "llvmpipe") && px[0] == 255 && px[1] == 0 && px[2] == 0 &&
                 px[3] == 255;
  printf("C llvmpipe clears and reads back (%s: %u %u %u %u) -> %s\n", renderer ? renderer : "no renderer",
         px[0], px[1], px[2], px[3], ok ? "OK" : "FAIL");

  /* Nothing of the host's GL in the process: every mapped GL library is one
   * of DIR's. */
  FILE *maps = fopen("/proc/self/maps", "r");
  char line[8192];
  int foreign = 0;
  while (maps && fgets(line, sizeof line, maps)) {
    const char *file = strchr(line, '/');
    if (!file) continue;
    if ((strstr(file, "libEGL") || strstr(file, "libGL") || strstr(file, "libgallium") ||
         strstr(file, "_dri.so") || strstr(file, "libglapi")) &&
        strncmp(file, argv[1], strlen(argv[1])) != 0) {
      printf("  foreign: %s", file);
      foreign = 1;
    }
  }
  if (maps) fclose(maps);
  printf("D no GL library of the host mapped -> %s\n", foreign ? "FAIL" : "OK");
  return ok && !foreign ? 0 : 1;
}
