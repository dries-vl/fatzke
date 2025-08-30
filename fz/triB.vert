#version 450
layout(location=0) out vec2 vUV;
void main() {
    const vec2 p[3] = vec2[3]( vec2(-1,-1), vec2(3,-1), vec2(-1,3) );
    vec2 pos = p[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    vUV = pos * 0.5 + 0.5;
}
