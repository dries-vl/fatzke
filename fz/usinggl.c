// cc tri_gl.c -lwayland-client -lwayland-egl -lEGL -lGLESv2 -O2 -o tri_gl && ./tri_gl
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef uint32_t u32;
typedef int32_t i32;

/* -------- Wayland (minimal) -------- */
static struct wl_display* dpy;
static struct wl_compositor* comp;
static struct xdg_wm_base* xdg;
static struct wl_surface* surf;
static struct xdg_surface* xsurf;
static struct xdg_toplevel* xtop;
static struct wl_egl_window* wegl;
static volatile int configured = 0;
static i32 win_w = 1280, win_h = 720;

static void xdg_ping(void* d, struct xdg_wm_base* b, u32 s)
{
    (void)d;
    xdg_wm_base_pong(b, s);
}

static const struct xdg_wm_base_listener xdg_wm_l = {.ping = xdg_ping};

static void top_cfg(void* d, struct xdg_toplevel* t, i32 w, i32 h, struct wl_array* st)
{
    (void)d;
    (void)t;
    (void)st;
    if (w > 0 && h > 0)
    {
        win_w = w;
        win_h = h;
        if (wegl) wl_egl_window_resize(wegl, win_w, win_h, 0, 0);
    }
}

static void top_close(void* d, struct xdg_toplevel* t)
{
    (void)d;
    (void)t;
    exit(0);
}

static void top_bounds(void* d, struct xdg_toplevel* t, i32 w, i32 h)
{
    (void)d;
    (void)t;
    (void)w;
    (void)h;
}

static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c)
{
    (void)d;
    (void)t;
    (void)c;
}

static const struct xdg_toplevel_listener top_l = {
    .configure = top_cfg, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
};

static void xsurf_conf(void* d, struct xdg_surface* s, u32 serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}

static const struct xdg_surface_listener xsurf_l = {.configure = xsurf_conf};

static void reg_add(void* d, struct wl_registry* r, u32 name, const char* iface, u32 ver)
{
    (void)d;
    (void)ver;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "xdg_wm_base"))
    {
        xdg = wl_registry_bind(r, name, &xdg_wm_base_interface, 6);
        xdg_wm_base_add_listener(xdg, &xdg_wm_l,NULL);
    }
}

static void reg_rem(void* d, struct wl_registry* r, u32 name)
{
    (void)d;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener reg_l = {.global = reg_add, .global_remove = reg_rem};

/* -------- EGL/GL -------- */
static EGLDisplay edpy;
static EGLConfig ecfg;
static EGLContext ectx;
static EGLSurface esurf;

static void die(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static GLuint mk_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src,NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s,GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(s, 512,NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint mk_prog(const char* vs, const char* fs)
{
    GLuint v = mk_shader(GL_VERTEX_SHADER, vs), f = mk_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p,GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(p, 512,NULL, log);
        fprintf(stderr, "link: %s\n", log);
        exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

int main(void)
{
    /* Wayland */
    dpy = wl_display_connect(NULL);
    if (!dpy) die("wl_connect");
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l,NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !xdg) die("missing compositor/xdg");
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(xdg, surf);
    xdg_surface_add_listener(xsurf, &xsurf_l,NULL);
    xtop = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(xtop, &top_l,NULL);
    xdg_toplevel_set_title(xtop, "tri_gl");
    wl_surface_commit(surf);
    while (!configured) wl_display_dispatch(dpy);
    wegl = wl_egl_window_create(surf, win_w, win_h);
    if (!wegl) die("wl_egl_window_create");

    /* EGL init */
    edpy = eglGetDisplay((EGLNativeDisplayType)dpy);
    if (edpy == EGL_NO_DISPLAY) die("eglGetDisplay");
    if (!eglInitialize(edpy,NULL,NULL)) die("eglInitialize");
    const EGLint cfg[] = {
        EGL_SURFACE_TYPE,EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(edpy, cfg, &ecfg, 1, &ncfg) || ncfg < 1) die("eglChooseConfig");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) die("eglBindAPI");
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,EGL_NONE};
    ectx = eglCreateContext(edpy, ecfg,EGL_NO_CONTEXT, ctx_attribs);
    if (ectx == EGL_NO_CONTEXT) die("eglCreateContext");
    esurf = eglCreateWindowSurface(edpy, ecfg, (EGLNativeWindowType)wegl,NULL);
    if (esurf == EGL_NO_SURFACE) die("eglCreateWindowSurface");
    if (!eglMakeCurrent(edpy, esurf, esurf, ectx)) die("eglMakeCurrent");

    /* --- Pass A resources: 100x100 RGBA texture as color attachment on an FBO --- */
    GLuint texA = 0, fboA = 0;
    glGenTextures(1, &texA);
    glBindTexture(GL_TEXTURE_2D, texA);
    glTexImage2D(GL_TEXTURE_2D, 0,GL_RGBA, 100, 100, 0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fboA);
    glBindFramebuffer(GL_FRAMEBUFFER, fboA);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D, texA, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("FBO incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* --- Programs --- */
    const char* vsA =
        "precision mediump float;\n"
        "attribute vec2 aPos; attribute vec3 aCol; varying vec3 vCol;\n"
        "void main(){ gl_Position=vec4(aPos,0.0,1.0); vCol=aCol; }\n";
    const char* fsA =
        "precision mediump float;\n"
        "varying vec3 vCol; void main(){ gl_FragColor=vec4(vCol,1.0); }\n";
    GLuint progA = mk_prog(vsA, fsA);
    GLint aPosA = glGetAttribLocation(progA, "aPos");
    GLint aColA = glGetAttribLocation(progA, "aCol");

    const char* vsB =
        "precision mediump float;\n"
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
        "void main(){ gl_Position=vec4(aPos,0.0,1.0); vUV=aUV; }\n";
    const char* fsB =
        "precision mediump float;\n"
        "uniform sampler2D uTex; varying vec2 vUV; void main(){ gl_FragColor=texture2D(uTex,vUV); }\n";
    GLuint progB = mk_prog(vsB, fsB);
    GLint aPosB = glGetAttribLocation(progB, "aPos");
    GLint aUVB = glGetAttribLocation(progB, "aUV");
    GLint uTex = glGetUniformLocation(progB, "uTex");

    /* Static geometry (client arrays; no VBO/VAO) */
    const GLfloat triA[] = {
        // xy + rgb (3 verts)
        -0.6f, -0.5f, 1, 0, 0,
        0.0f, 0.6f, 0, 1, 0,
        0.6f, -0.5f, 0, 0, 1
    };
    const GLfloat triB[] = {
        // fullscreen tri: xy + uv (3 verts)
        -1.0f, -1.0f, 0.0f, 0.0f,
        3.0f, -1.0f, 2.0f, 0.0f,
        -1.0f, 3.0f, 0.0f, 2.0f
    };

    /* Loop */
    for (;;)
    {
        while (wl_display_dispatch_pending(dpy) > 0)
        {
        } /* drain events */

        /* Pass A: draw tiny RGB triangle to 100x100 texture */
        glBindFramebuffer(GL_FRAMEBUFFER, fboA);
        glViewport(0, 0, 100, 100);
        glUseProgram(progA);
        glEnableVertexAttribArray(aPosA);
        glEnableVertexAttribArray(aColA);
        glVertexAttribPointer(aPosA, 2,GL_FLOAT,GL_FALSE, 5 * sizeof(GLfloat), triA + 0);
        glVertexAttribPointer(aColA, 3,GL_FLOAT,GL_FALSE, 5 * sizeof(GLfloat), triA + 2);
        glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(aPosA);
        glDisableVertexAttribArray(aColA);

        /* Pass B: sample that texture to the window */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, win_w, win_h);
        glUseProgram(progB);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texA);
        glUniform1i(uTex, 0);
        glEnableVertexAttribArray(aPosB);
        glEnableVertexAttribArray(aUVB);
        glVertexAttribPointer(aPosB, 2,GL_FLOAT,GL_FALSE, 4 * sizeof(GLfloat), triB + 0);
        glVertexAttribPointer(aUVB, 2,GL_FLOAT,GL_FALSE, 4 * sizeof(GLfloat), triB + 2);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(aPosB);
        glDisableVertexAttribArray(aUVB);

        eglSwapBuffers(edpy, esurf);
    }
}
