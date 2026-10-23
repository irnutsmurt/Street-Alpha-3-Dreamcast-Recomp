#version 450
layout(location = 0) out vec2 v_uv;
layout(push_constant) uniform PC {
    vec4 rect;    // x, y, w, h in NDC
    vec4 params;  // scanline strength (0 = off), scanline count, mode (0 game, 1 overlay), picture height in pixels
    vec4 color;   // gamma exponent, saturation, unused, TV levels (1 = on)
} pc;
void main() {
    vec2 p = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    v_uv = p;
    gl_Position = vec4(pc.rect.xy + p * pc.rect.zw, 0.0, 1.0);
}
