#version 450
layout(location=0) out vec3 vColor;
void main() {
    const vec2 pos[3] = vec2[3]( vec2(-0.5,-0.5), vec2(0.0,0.5), vec2(0.5,-0.5) );
    vec2 p = pos[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);
    vColor = vec3(p * 0.5 + 0.5, 1.0);
}
