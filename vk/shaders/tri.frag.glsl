#version 450
layout(location = 0) out vec4 o_color;

// Clamp to [0,1] vs your WGSL's (100, .4, .1, 1)
void main() {
    o_color = vec4(1.0, 0.4, 0.1, 1.0);
}
