import math, ctypes, numpy as np, pyglet, moderngl as mgl
from pyglet.window import key

W, H = 1280, 720
INTERNAL_SCALE = 0.7
N_INST = 4096  # many instances; GPU will cull

# ---------- window + GL context ----------
win = pyglet.window.Window(W, H, caption='GPU-driven', vsync=True, resizable=True)
ctx = mgl.create_context(require=430)  # OpenGL 4.3+ for compute

# ---------- geometry: a unit cube ----------
cube_pos = np.array([
    # x,y,z
    -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,   # back
    -1,-1, 1,  1,-1, 1,  1, 1, 1,  -1, 1, 1,  # front
], dtype='f4').reshape(-1,3)
cube_idx = np.array([
    0,1,2, 2,3,0,    4,5,6, 6,7,4,     # back, front
    0,4,7, 7,3,0,    1,5,6, 6,2,1,     # left, right
    3,2,6, 6,7,3,    0,1,5, 5,4,0      # top, bottom
], dtype='u4')
INDEX_COUNT = cube_idx.size

vb = ctx.buffer(cube_pos.tobytes())
ib = ctx.buffer(cube_idx.tobytes())

# ---------- instance data ----------
# Each instance: world matrix (mat4) + centerRadius (vec4)
def grid_instances(n):
    side = int(math.sqrt(n))
    ii = []
    spacing = 4.0
    for z in range(side):
        for x in range(side):
            if len(ii) == n: break
            tx = (x - side/2)*spacing
            tz = (z - side/2)*spacing
            s = 0.5
            M = np.eye(4, dtype='f4')
            M[0,0]=s; M[1,1]=s; M[2,2]=s
            M[3,:3] = (tx, 0.0, tz)
            center = np.array([0,0,0], dtype='f4')  # unit cube centered at 0
            radius = s*math.sqrt(3)                 # ~bbox sphere
            ii.append((M, np.array([*center, radius], dtype='f4')))
    return ii

inst_data = grid_instances(N_INST)
inst_struct = np.zeros(N_INST, dtype=[('m', 'f4', (4,4)), ('cr','f4',4)])
for i,(M,CR) in enumerate(inst_data):
    inst_struct['m'][i]  = M
    inst_struct['cr'][i] = CR
instances_ssbo = ctx.buffer(inst_struct.tobytes())

# Visible index list (u32) + counter (u32) + indirect cmd (5*u32)
visible_ssbo  = ctx.buffer(reserve=N_INST*4)
counter_ssbo  = ctx.buffer(reserve=4)         # uint counter
# OpenGL DrawElementsIndirectCommand = { count, primCount, firstIndex, baseVertex, baseInstance }
indirect_ssbo = ctx.buffer(np.array([INDEX_COUNT,0,0,0,0], dtype='u4').tobytes())  # will update primCount

# ---------- camera uniform ----------
# mat4 viewProj + vec4(camPos)
cam_buf = ctx.buffer(reserve=4*4*4 + 16)

def perspective(fovy, aspect, znear, zfar):
    f = 1.0/math.tan(fovy/2)
    M = np.zeros((4,4), dtype='f4')
    M[0,0]=f/aspect; M[1,1]=f
    M[2,2]=(zfar+znear)/(znear-zfar)
    M[2,3]=-1.0
    M[3,2]=(2*zfar*znear)/(znear-zfar)
    return M

def look_at(eye, target, up):
    f = (target-eye); f/=np.linalg.norm(f)
    s = np.cross(f, up); s/=np.linalg.norm(s)
    u = np.cross(s, f)
    M = np.eye(4, dtype='f4')
    M[0,:3]=s; M[1,:3]=u; M[2,:3]=-f
    T = np.eye(4, dtype='f4'); T[3,:3] = -eye
    return M @ T

cam_pos = np.array([0.0, 3.0, 18.0], dtype='f4')
cam_yaw = 0.0

def update_camera():
    aspect = max(win.width,1)/max(win.height,1)
    P = perspective(math.radians(60), aspect, 0.1, 200.0)
    forward = np.array([math.sin(cam_yaw), 0.0, -math.cos(cam_yaw)], dtype='f4')
    target = cam_pos + forward
    V = look_at(cam_pos, target, np.array([0,1,0], dtype='f4'))
    VP = V @ P  # note: row-major here; we’ll transpose for GLSL column-major
    packed = np.ascontiguousarray(VP.T.astype('f4'))
    pad = np.array([*cam_pos, 0.0], dtype='f4').tobytes()
    cam_buf.write(packed.tobytes() + pad)

# ---------- offscreen framebuffer ----------
def make_offscreen():
    iw, ih = max(1,int(win.width*INTERNAL_SCALE)), max(1,int(win.height*INTERNAL_SCALE))
    color = ctx.texture((iw, ih), 4, dtype='f2')  # rgba16f
    depth = ctx.depth_renderbuffer((iw, ih))
    fbo = ctx.framebuffer(color, depth)
    return fbo, color
scene_fbo, scene_tex = make_offscreen()

# ---------- shaders ----------
compute_src = """
#version 430
layout(local_size_x = 64) in;

struct Instance {
    mat4 M;
    vec4 CR; // xyz center, w radius (world)
};

layout(std430, binding=0) readonly buffer Instances { Instance inst[]; };
layout(std430, binding=1) writeonly buffer Visible   { uint visible[]; };
layout(std430, binding=2) buffer Counter             { uint cnt; };
layout(std430, binding=3) buffer IndirectCmd        { uint indirect[5]; };
// [count, primCount, firstIndex, baseVertex, baseInstance]

layout(std140, binding=4) uniform Camera {
    mat4 viewProj;
    vec4 camPos;
};

bool sphere_frustum(vec3 c, float r) {
    // Cheap: clip-space w test against -w..w after transforming center
    vec4 h = viewProj * vec4(c,1.0);
    float aw = abs(h.w);
    // Near/far
    if (h.z < -aw - r || h.z > aw + r) return false;
    // Left/right/top/bottom via x/y against w
    if (h.x < -aw - r || h.x > aw + r) return false;
    if (h.y < -aw - r || h.y > aw + r) return false;
    return true;
}

void main() {
    uint i = gl_GlobalInvocationID.x;

    // Thread 0 seeds indirect and counter (benign races on same values)
    if (i == 0u) {
        cnt = 0u;
        indirect[0] = indirect[0]; // count already set
        indirect[1] = 0u;          // primCount will be set in finalize pass
        indirect[2] = 0u; indirect[3] = 0u; indirect[4] = 0u;
    }

    if (i >= inst.length()) return;

    vec3 c = (inst[i].M * vec4(inst[i].CR.xyz, 1.0)).xyz;
    float r = inst[i].CR.w * length(inst[i].M[0].xyz); // assume uniform-ish scale
    if (!sphere_frustum(c, r)) return;

    uint w = atomicAdd(cnt, 1u);
    visible[w] = i;
}
"""

finalize_src = """
#version 430
layout(local_size_x = 1) in;
layout(std430, binding=2) buffer Counter      { uint cnt; };
layout(std430, binding=3) buffer IndirectCmd  { uint indirect[5]; };
void main() { indirect[1] = cnt; }
"""

scene_vs = """
#version 430
layout(location=0) in vec3 in_pos;

struct Instance { mat4 M; vec4 CR; };
layout(std430, binding=0) readonly buffer Instances { Instance inst[]; };
layout(std430, binding=1) readonly buffer Visible   { uint visible[]; };
layout(std140, binding=4) uniform Camera {
    mat4 viewProj;
    vec4 camPos;
};

void main() {
    uint compact_idx = visible[gl_InstanceID];
    mat4 M = inst[compact_idx].M;
    gl_Position = viewProj * (M * vec4(in_pos, 1.0));
}
"""

scene_fs = """
#version 430
out vec4 fragColor;
void main() {
    // flat shaded color
    fragColor = vec4(0.85, 0.9, 1.0, 1.0);
}
"""

post_vs = """
#version 430
out vec2 uv;
void main(){
    vec2 p = vec2((gl_VertexID==2)?3.0:-1.0, (gl_VertexID==0)?-3.0:1.0);
    uv = 0.5 * (p + 1.0);
    gl_Position = vec4(p,0,1);
}
"""

post_fs = """
#version 430
in vec2 uv;
out vec4 o;
uniform sampler2D sceneTex;
void main(){
    vec3 c = texture(sceneTex, uv).rgb;
    // simple tonemap
    c = c/(c+vec3(1.0));
    o = vec4(pow(c, vec3(1.0/2.2)), 1.0);
}
"""

comp = ctx.compute_shader(compute_src)
finalize = ctx.compute_shader(finalize_src)
prog = ctx.program(vertex_shader=scene_vs, fragment_shader=scene_fs)
post = ctx.program(vertex_shader=post_vs, fragment_shader=post_fs)

vao = ctx.vertex_array(prog, [(vb, '3f', 'in_pos')], index_buffer=ib)
# Bind SSBO indices to match shader bindings
instances_ssbo.bind_to_storage_buffer(0)
visible_ssbo.bind_to_storage_buffer(1)
counter_ssbo.bind_to_storage_buffer(2)
indirect_ssbo.bind_to_storage_buffer(3)
cam_buf.bind_to_uniform_block(4)

# ---------- input ----------
keys = key.KeyStateHandler()
win.push_handlers(keys)

speed = 12.0
def handle_input(dt):
    global cam_pos, cam_yaw
    if keys[key.LEFT]:  cam_yaw -= 1.2*dt
    if keys[key.RIGHT]: cam_yaw += 1.2*dt
    forward = np.array([math.sin(cam_yaw), 0.0, -math.cos(cam_yaw)], dtype='f4')
    right   = np.array([ forward[2], 0.0, -forward[0] ], dtype='f4')
    v = np.zeros(3, dtype='f4')
    if keys[key.UP]:    v += forward
    if keys[key.DOWN]:  v -= forward
    if keys[key.A]:     v -= right
    if keys[key.D]:     v += right
    if keys[key.W]:     v += np.array([0,1,0], dtype='f4')
    if keys[key.S]:     v -= np.array([0,1,0], dtype='f4')
    n = np.linalg.norm(v)
    if n > 0: cam_pos += (v/n) * (speed*dt)

# ---------- resize ----------
@win.event
def on_resize(w, h):
    global scene_fbo, scene_tex
    scene_fbo.release(); scene_tex.release()
    scene_fbo, scene_tex = make_offscreen()

# ---------- frame ----------
def render(dt):
    handle_input(dt)
    update_camera()

    # COMPUTE: cull + compact IDs
    groups = (N_INST + 63)//64
    comp.run(groups, 1, 1)
    # finalize primCount = visible_count
    finalize.run(1,1,1)

    # SCENE: offscreen
    scene_fbo.use()
    ctx.enable(mgl.DEPTH_TEST)
    scene_fbo.clear(0.02, 0.02, 0.03, 1.0, depth=1.0)
    # draw indirect: buffer contains 5*u32
    vao.render_indirect(mgl.TRIANGLES, indirect_ssbo, count=1)

    # POST: to swapchain size
    win.use()
    ctx.disable(mgl.DEPTH_TEST)
    ctx.screen.clear(0.0,0.0,0.0,1.0)
    scene_tex.use(0)
    post['sceneTex'] = 0
    ctx.screen.draw(vao=None, vertices=3, program=post)

pyglet.clock.schedule_interval(render, 1.0/120.0)
pyglet.app.run()
