#version 450
// PowerVR screen-space vertices: x/y in framebuffer pixels, z = 1/w.
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_base;    // unpacked from BGRA8 (ARGB8888 little-endian)
layout(location = 3) in vec4 in_offset;
layout(location = 4) in vec4 in_uvbox;   // its polygon's texture coordinates' box (pvr.frag)

layout(push_constant) uniform PC {
    vec2 scale;        // 2/width, 2/height of the guest framebuffer
    float zscale;      // maps 1/w into [0,1]
    uint flags;
    float alpha_ref;
    float xoff;        // added to x: the wide picture's margin (SFA3_WIDE), 0 normally
    float pad1, pad2;
    vec4 fog_color;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_base;
layout(location = 2) out vec4 v_offset;
layout(location = 3) flat out vec4 v_uvbox;

void main() {
    float invw = max(in_pos.z, 1e-8);
    float w = 1.0 / invw;
    vec2 ndc = (in_pos.xy + vec2(pc.xoff, 0.0)) * pc.scale - 1.0;
    // depth linear in screen space (1/w), perspective-correct varyings via w
    gl_Position = vec4(ndc * w, clamp(invw * pc.zscale, 0.0, 1.0) * w, w);
    v_uv = in_uv;
    v_base = in_base;
    v_offset = in_offset;
    v_uvbox = in_uvbox;
}
