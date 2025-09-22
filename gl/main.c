// Build: tcc main.c -o gpu -lX11 -lGL -lm
#define _GNU_SOURCE
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if(!(x)){ fprintf(stderr,"CHECK failed: %s:%d\n",__FILE__,__LINE__); exit(1);} } while(0)
#define GLCHK() do { GLenum e; while((e=glGetError())) fprintf(stderr,"GL err 0x%x at %s:%d\n",e,__FILE__,__LINE__); } while(0)

static Display *dpy;
static Window win;
static GLXContext ctx;
static int win_w = 1280, win_h = 720;
static float INTERNAL_SCALE = 0.7f;

// -------- Minimal GL loader via glXGetProcAddress --------
#define GLFUNC(ret, name, ...) typedef ret (*PFN##name)(__VA_ARGS__); static PFN##name name;
#define LOAD(name) do { name=(PFN##name)glXGetProcAddressARB((const GLubyte*)#name); if(!name){fprintf(stderr,"Missing GL function %s\n",#name); exit(1);} } while(0)

GLFUNC(void, glGenVertexArrays, GLsizei, GLuint*)
GLFUNC(void, glBindVertexArray, GLuint)
GLFUNC(void, glGenBuffers, GLsizei, GLuint*)
GLFUNC(void, glBindBuffer, GLenum, GLuint)
GLFUNC(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)
GLFUNC(void, glBufferSubData, GLenum, GLintptr, GLsizeiptr, const void*)
GLFUNC(void, glBindBufferBase, GLenum, GLuint, GLuint)
GLFUNC(void, glBindBufferRange, GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)
GLFUNC(void, glGenRenderbuffers, GLsizei, GLuint*)
GLFUNC(void, glBindRenderbuffer, GLenum, GLuint)
GLFUNC(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)
GLFUNC(void, glGenFramebuffers, GLsizei, GLuint*)
GLFUNC(void, glBindFramebuffer, GLenum, GLuint)
GLFUNC(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFUNC(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)
GLFUNC(GLenum, glCheckFramebufferStatus, GLenum)
GLFUNC(void, glDeleteFramebuffers, GLsizei, const GLuint*)
GLFUNC(void, glDeleteRenderbuffers, GLsizei, const GLuint*)

GLFUNC(GLuint, glCreateShader, GLenum)
GLFUNC(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)
GLFUNC(void, glCompileShader, GLuint)
GLFUNC(void, glGetShaderiv, GLuint, GLenum, GLint*)
GLFUNC(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFUNC(GLuint, glCreateProgram, void)
GLFUNC(void, glAttachShader, GLuint, GLuint)
GLFUNC(void, glLinkProgram, GLuint)
GLFUNC(void, glGetProgramiv, GLuint, GLenum, GLint*)
GLFUNC(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFUNC(void, glDeleteShader, GLuint)
GLFUNC(void, glUseProgram, GLuint)
GLFUNC(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLFUNC(void, glUniform1i, GLint, GLint)

GLFUNC(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)
GLFUNC(void, glEnableVertexAttribArray, GLuint)
GLFUNC(void, glDrawElementsIndirect, GLenum, GLenum, const void*)

GLFUNC(void, glDispatchCompute, GLuint, GLuint, GLuint)
GLFUNC(void, glMemoryBarrier, GLbitfield)

GLFUNC(GLuint, glGetUniformBlockIndex, GLuint, const GLchar*)
GLFUNC(void, glUniformBlockBinding, GLuint, GLuint, GLuint)
GLFUNC(void*, glMapBuffer, GLenum, GLenum)
GLFUNC(GLboolean, glUnmapBuffer, GLenum)

static void load_gl_funcs() {
    LOAD(glGenVertexArrays); LOAD(glBindVertexArray);
    LOAD(glGenBuffers); LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glBufferSubData);
    LOAD(glMapBuffer); LOAD(glUnmapBuffer);
    LOAD(glBindBufferBase); LOAD(glBindBufferRange);
    LOAD(glGenRenderbuffers); LOAD(glBindRenderbuffer); LOAD(glRenderbufferStorage);
    LOAD(glGenFramebuffers); LOAD(glBindFramebuffer); LOAD(glFramebufferTexture2D);
    LOAD(glFramebufferRenderbuffer); LOAD(glCheckFramebufferStatus);
    LOAD(glDeleteFramebuffers);LOAD(glDeleteRenderbuffers);

    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glDeleteShader); LOAD(glUseProgram);
    LOAD(glGetUniformLocation); LOAD(glUniform1i);

    LOAD(glVertexAttribPointer); LOAD(glEnableVertexAttribArray);
    LOAD(glDrawElementsIndirect);

    LOAD(glDispatchCompute); LOAD(glMemoryBarrier);

    LOAD(glGetUniformBlockIndex); LOAD(glUniformBlockBinding);
}

// --------- GLX 4.3 core context creation ----------
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
static GLXContext create_glx_context(Display* dpy, GLXFBConfig fb, int major, int minor) {
    PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB =
        (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    CHECK(glXCreateContextAttribsARB);
    int attribs[] = {
        0x2091/*GLX_CONTEXT_MAJOR_VERSION_ARB*/, major,
        0x2092/*GLX_CONTEXT_MINOR_VERSION_ARB*/, minor,
        0x9126/*GLX_CONTEXT_PROFILE_MASK_ARB*/, 0x00000001/*CORE*/,
        0x2094/*GLX_CONTEXT_FLAGS_ARB*/, 0, // could request debug
        0
    };
    return glXCreateContextAttribsARB(dpy, fb, 0, True, attribs);
}

static GLXFBConfig choose_fbconfig(Display* d) {
    int visual_attribs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE,   8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, True,
        None
    };
    int n;
    GLXFBConfig* cfgs = glXChooseFBConfig(d, DefaultScreen(d), visual_attribs, &n);
    CHECK(cfgs && n>0);
    GLXFBConfig fb = cfgs[0];
    XFree(cfgs);
    return fb;
}

// ----------------- Tiny math -----------------
typedef struct { float m[16]; } mat4; // column-major for GLSL
static mat4 mat4_identity() { mat4 r={{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}; return r; }
static mat4 mat4_mul(mat4 a, mat4 b){ mat4 r; for(int c=0;c<4;c++)for(int r0=0;r0<4;r0++){ r.m[c*4+r0]=a.m[0*4+r0]*b.m[c*4+0]+a.m[1*4+r0]*b.m[c*4+1]+a.m[2*4+r0]*b.m[c*4+2]+a.m[3*4+r0]*b.m[c*4+3]; } return r; }
static mat4 mat4_perspective(float fovy, float aspect, float znear, float zfar){
    float f=1.0f/tanf(fovy*0.5f);
    mat4 r={{0}};
    r.m[0]=f/aspect; r.m[5]=f; r.m[10]=(zfar+znear)/(znear-zfar); r.m[11]=-1.0f; r.m[14]=(2*zfar*znear)/(znear-zfar);
    return r;
}
static mat4 mat4_look(float ex,float ey,float ez, float tx,float ty,float tz){
    float fx=tx-ex, fy=ty-ey, fz=tz-ez; float fl=sqrtf(fx*fx+fy*fy+fz*fz); fx/=fl; fy/=fl; fz/=fl;
    float upx=0, upy=1, upz=0;
    float sx=fy*upz - fz*upy, sy=fz*upx - fx*upz, sz=fx*upy - fy*upx;
    float sl=sqrtf(sx*sx+sy*sy+sz*sz); sx/=sl; sy/=sl; sz/=sl;
    float ux=sy*fz - sz*fy, uy=sz*fx - sx*fz, uz=sx*fy - sy*fx;
    mat4 M={{ sx, ux,-fx, 0,
              sy, uy,-fy, 0,
              sz, uz,-fz, 0,
             0,  0,  0,  1 }};
    mat4 T={{1,0,0,0, 0,1,0,0, 0,0,1,0, -ex,-ey,-ez,1}};
    return mat4_mul(T, M); // Note: we pack as column-major later; this is fine since we only send final VP.
}

// ---------------- Shaders ----------------
static const char* COMP_SRC =
"#version 430\n"
"layout(local_size_x=64) in;\n"
"struct Instance { mat4 M; vec4 CR; };\n"
"layout(std430,binding=0) readonly buffer Instances { Instance inst[]; };\n"
"layout(std430,binding=1) writeonly buffer Visible { uint visible[]; };\n"
"layout(std430,binding=2) buffer Counter { uint cnt; };\n"
"layout(std430,binding=3) buffer Indirect { uint indirect[5]; };\n"
"layout(std140,binding=4) uniform Camera { mat4 viewProj; vec4 camPos; };\n"
"bool sphere_frustum(vec3 c, float r){ vec4 h=viewProj*vec4(c,1); float aw=abs(h.w);"
" if(h.z<-aw-r||h.z>aw+r) return false; if(h.x<-aw-r||h.x>aw+r) return false; if(h.y<-aw-r||h.y>aw+r) return false; return true; }\n"
"void main(){ uint i=gl_GlobalInvocationID.x; if(i==0u){ cnt=0u; indirect[1]=0u; } if(i>=inst.length()) return;"
" vec3 c=(inst[i].M*vec4(inst[i].CR.xyz,1)).xyz; float r=inst[i].CR.w*length(inst[i].M[0].xyz);"
" if(!sphere_frustum(c,r)) return; uint w=atomicAdd(cnt,1u); visible[w]=i; }\n";

static const char* FINALIZE_SRC =
"#version 430\n"
"layout(local_size_x=1) in;\n"
"layout(std430,binding=2) buffer Counter { uint cnt; };\n"
"layout(std430,binding=3) buffer Indirect { uint indirect[5]; };\n"
"void main(){ indirect[1]=cnt; }\n";

static const char* VS_SRC =
"#version 430\n"
"layout(location=0) in vec3 in_pos;\n"
"struct Instance { mat4 M; vec4 CR; };\n"
"layout(std430,binding=0) readonly buffer Instances { Instance inst[]; };\n"
"layout(std430,binding=1) readonly buffer Visible { uint visible[]; };\n"
"layout(std140,binding=4) uniform Camera { mat4 viewProj; vec4 camPos; };\n"
"void main(){ uint id=visible[gl_InstanceID]; mat4 M=inst[id].M; gl_Position = viewProj * (M*vec4(in_pos,1)); }\n";

static const char* FS_SRC =
"#version 430\n"
"out vec4 o; void main(){ o=vec4(0.85,0.9,1.0,1.0); }\n";

static const char* POST_VS =
"#version 430\n"
"out vec2 uv; void main(){ vec2 p = vec2((gl_VertexID==2)?3.0:-1.0,(gl_VertexID==0)?-3.0:1.0);"
" uv = 0.5*(p+1.0); gl_Position=vec4(p,0,1); }\n";

static const char* POST_FS =
"#version 430\n"
"in vec2 uv; out vec4 o; uniform sampler2D sceneTex;"
"void main(){ vec3 c=texture(sceneTex,uv).rgb; c=c/(c+vec3(1.0)); o=vec4(pow(c,vec3(1.0/2.2)),1.0); }\n";

// ------------- Shader utils -------------
static GLuint make_shader(GLenum type, const char* src){
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,NULL);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetShaderInfoLog(s,4096,&n,log); fprintf(stderr,"Shader compile error:\n%.*s\n",n,log); exit(1); }
    return s;
}
static GLuint make_program(const char* vs, const char* fs){
    GLuint v=make_shader(GL_VERTEX_SHADER,vs);
    GLuint f=make_shader(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram();
    glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetProgramInfoLog(p,4096,&n,log); fprintf(stderr,"Link error:\n%.*s\n",n,log); exit(1); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
static GLuint make_compute(const char* cs){
    GLuint c=make_shader(GL_COMPUTE_SHADER,cs);
    GLuint p=glCreateProgram(); glAttachShader(p,c); glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetProgramInfoLog(p,4096,&n,log); fprintf(stderr,"Compute link error:\n%.*s\n",n,log); exit(1); }
    glDeleteShader(c); return p;
}

// ------------- Geometry (unit cube) -------------
static const float cube_pos[] = {
    -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,
    -1,-1, 1,  1,-1, 1,  1, 1, 1,  -1, 1, 1,
};
static const uint32_t cube_idx[] = {
    0,1,2,2,3,0,  4,5,6,6,7,4,
    0,4,7,7,3,0,  1,5,6,6,2,1,
    3,2,6,6,7,3,  0,1,5,5,4,0
};
#define INDEX_COUNT (sizeof(cube_idx)/sizeof(cube_idx[0]))

// ------------- Global GL objects -------------
static GLuint vao, vbo, ebo;
static GLuint prog, post_prog, comp_prog, finalize_prog;
static GLuint instances_ssbo, visible_ssbo, counter_ssbo, indirect_ssbo;
static GLuint cam_ubo;
static GLuint fbo, color_tex, depth_rb;

static const int N_INST = 4096;

// Instance struct: mat4 + vec4(centerRadius)
typedef struct { float M[16]; float CR[4]; } Instance;

static void create_offscreen(int w, int h){
    if(fbo){ glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&color_tex); glDeleteRenderbuffers(1,&depth_rb); }
    int iw = (int)(w * INTERNAL_SCALE); if(iw<1) iw=1;
    int ih = (int)(h * INTERNAL_SCALE); if(ih<1) ih=1;

    glGenTextures(1,&color_tex);
    glBindTexture(GL_TEXTURE_2D,color_tex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,iw,ih,0,GL_RGBA,GL_HALF_FLOAT,NULL);

    glGenRenderbuffers(1,&depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, iw, ih);

    glGenFramebuffers(1,&fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ------------- Setup scene -------------
static void setup_scene(){
    // VAO + buffers
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube_pos), cube_pos, GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glGenBuffers(1,&ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cube_idx), cube_idx, GL_STATIC_DRAW);

    // Programs
    prog = make_program(VS_SRC, FS_SRC);
    post_prog = make_program(POST_VS, POST_FS);
    comp_prog = make_compute(COMP_SRC);
    finalize_prog = make_compute(FINALIZE_SRC);

    // Instances buffer
    glGenBuffers(1,&instances_ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, instances_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, N_INST * sizeof(Instance), NULL, GL_STATIC_DRAW);

    // Fill instances as a grid
    Instance* inst = (Instance*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
    int side = (int)sqrtf((float)N_INST);
    float spacing = 4.0f;
    int k=0;
    for(int z=0; z<side && k<N_INST; ++z){
        for(int x=0; x<side && k<N_INST; ++x){
            float s=0.5f;
            float tx=(x - side/2)*spacing, tz=(z - side/2)*spacing;
            Instance I = {0};
            // column-major identity scaled
            I.M[0]=s; I.M[5]=s; I.M[10]=s; I.M[15]=1.0f;
            I.M[12]=tx; I.M[13]=0.0f; I.M[14]=tz; // translation in last column
            I.CR[0]=0; I.CR[1]=0; I.CR[2]=0; I.CR[3]=s*1.73205f;
            inst[k++]=I;
        }
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    // Visible list + counter + indirect cmd
    glGenBuffers(1,&visible_ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, visible_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, N_INST*sizeof(uint32_t), NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1,&counter_ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_ssbo);
    uint32_t zero=0; glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(uint32_t), &zero, GL_DYNAMIC_DRAW);

    glGenBuffers(1,&indirect_ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, indirect_ssbo);
    uint32_t cmd[5] = { (uint32_t)INDEX_COUNT, 0, 0, 0, 0 };
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(cmd), cmd, GL_DYNAMIC_DRAW);

    // Bind to slots
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instances_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, visible_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, counter_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, indirect_ssbo);

    // Camera UBO
    glGenBuffers(1,&cam_ubo); glBindBuffer(GL_UNIFORM_BUFFER, cam_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(float)*(16+4), NULL, GL_DYNAMIC_DRAW);
    GLuint cam_idx = glGetUniformBlockIndex(prog, "Camera");
    glUniformBlockBinding(prog, cam_idx, 4);
    GLuint cam_idx2 = glGetUniformBlockIndex(comp_prog, "Camera");
    glUniformBlockBinding(comp_prog, cam_idx2, 4);
    // Bind same UBO to binding=4
    glBindBufferBase(GL_UNIFORM_BUFFER, 4, cam_ubo);

    // Post sampler uniform
    glUseProgram(post_prog);
    GLint loc = glGetUniformLocation(post_prog, "sceneTex");
    if(loc>=0) glUniform1i(loc, 0);

    create_offscreen(win_w, win_h);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
}

// --------- Camera state ---------
static float cam_x=0.0f, cam_y=3.0f, cam_z=18.0f, cam_yaw=0.0f;
static bool key_left=false, key_right=false, key_up=false, key_down=false;
static bool key_A=false, key_D=false, key_W=false, key_S=false;

static void update_camera_ubo(){
    float aspect = (float)win_w/(float)win_h;
    mat4 P = mat4_perspective(60.0f*(float)M_PI/180.0f, aspect, 0.1f, 200.0f);
    float fx = sinf(cam_yaw), fz = -cosf(cam_yaw);
    float tx = cam_x + fx, ty = cam_y, tz = cam_z + fz;
    mat4 V = mat4_look(cam_x,cam_y,cam_z, tx,ty,tz);
    mat4 VP = mat4_mul(V, P); // our math is row*col; we stored column-major in arrays already
    glBindBuffer(GL_UNIFORM_BUFFER, cam_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(VP.m), VP.m);
    float camPos[4] = { cam_x, cam_y, cam_z, 0.0f };
    glBufferSubData(GL_UNIFORM_BUFFER, sizeof(VP.m), sizeof(camPos), camPos);
}

// ------------- Frame -------------
static void frame(float dt){
    // input
    float speed = 12.0f;
    if(key_left)  cam_yaw -= 1.2f*dt;
    if(key_right) cam_yaw += 1.2f*dt;
    float fx = sinf(cam_yaw), fz = -cosf(cam_yaw);
    float rx =  fz, rz = -fx;
    float vx=0, vy=0, vz=0;
    if(key_up)    { vx+=fx; vz+=fz; }
    if(key_down)  { vx-=fx; vz-=fz; }
    if(key_A)     { vx-=rx; vz-=rz; }
    if(key_D)     { vx+=rx; vz+=rz; }
    if(key_W)     { vy+=1.0f; }
    if(key_S)     { vy-=1.0f; }
    float len = sqrtf(vx*vx+vy*vy+vz*vz);
    if(len>0){ vx/=len; vy/=len; vz/=len; }
    cam_x += vx*speed*dt; cam_y += vy*speed*dt; cam_z += vz*speed*dt;

    update_camera_ubo();

    // COMPUTE: cull + compact
    glUseProgram(comp_prog);
    glDispatchCompute((GLuint)((N_INST+63)/64), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Finalize: copy visible count -> indirect.primCount
    glUseProgram(finalize_prog);
    glDispatchCompute(1,1,1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    // SCENE: render to offscreen
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    int iw=(int)(win_w*INTERNAL_SCALE), ih=(int)(win_h*INTERNAL_SCALE);
    if(iw<1) iw=1; if(ih<1) ih=1;
    glViewport(0,0,iw,ih);
    glClearColor(0.02f,0.02f,0.03f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(prog);
    glBindVertexArray(vao);
    // indirect buffer lives as SSBO bound at 3; also bind it to DRAW_INDIRECT
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect_ssbo);
    glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)0);

    // POST: blit/tonemap to window
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0,0,win_w,win_h);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(post_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
}

// ------------- X11 helpers -------------
static Atom WM_DELETE_WINDOW;

static void handle_key(XKeyEvent* e, bool down){
    KeySym ks = XLookupKeysym(e, 0);
    switch(ks){
        case XK_Left:  key_left=down; break;
        case XK_Right: key_right=down; break;
        case XK_Up:    key_up=down; break;
        case XK_Down:  key_down=down; break;
        case XK_a: case XK_A: key_A=down; break;
        case XK_d: case XK_D: key_D=down; break;
        case XK_w: case XK_W: key_W=down; break;
        case XK_s: case XK_S: key_S=down; break;
        case XK_Escape: exit(0);
        default: break;
    }
}

int main(){
    // X11 window + GLX context
    dpy = XOpenDisplay(NULL); CHECK(dpy);
    int scr = DefaultScreen(dpy);
    GLXFBConfig fb = choose_fbconfig(dpy);
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fb); CHECK(vi);

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0,0, win_w,win_h, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "GPU-driven (X11 + GL4.3)");
    WM_DELETE_WINDOW = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &WM_DELETE_WINDOW, 1);
    XMapWindow(dpy, win);

    ctx = create_glx_context(dpy, fb, 4, 3); CHECK(ctx);
    CHECK(glXMakeCurrent(dpy, win, ctx));

    // Load GL functions
    load_gl_funcs();

    setup_scene();

    double last = 0.0;
    for(;;){
        while(XPending(dpy)){
            XEvent ev; XNextEvent(dpy, &ev);
            if(ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == WM_DELETE_WINDOW) goto quit;
            if(ev.type == ConfigureNotify){
                win_w = ev.xconfigure.width; win_h = ev.xconfigure.height;
                create_offscreen(win_w, win_h);
            }
            if(ev.type == KeyPress)   handle_key(&ev.xkey, true);
            if(ev.type == KeyRelease) handle_key(&ev.xkey, false);
        }
        // basic dt (not vsynced by GLX here; swap interval not set — simplest path)
        static struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec*1e-9;
        double dt = (last==0.0) ? (1.0/120.0) : (now-last);
        last = now;

        frame((float)dt);

        glXSwapBuffers(dpy, win);
    }

quit:
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
