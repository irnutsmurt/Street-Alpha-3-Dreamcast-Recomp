#version 450
// 16:9 background extension (vk_renderer.c, widescreen.c): a quad over the picture's columns and rows a background
// layer lacks art in (quad.x to quad.y, rows.x to rows.y); ext.frag sets the depth.
layout(push_constant) uniform PC {
    vec4 map;
    vec4 layer;
    vec4 quad;
    vec4 shade;
    vec4 rows;
} pc;
void main() {
    vec2 p = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(mix(pc.quad.x, pc.quad.y, p.x), mix(pc.rows.x, pc.rows.y, p.y), pc.layer.y, 1.0);
}
