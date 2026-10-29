#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_base;
layout(location = 2) in vec4 v_offset;
layout(location = 3) flat in vec4 v_uvbox;  // the polygon's texture coordinates' box
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PC {
    vec2 scale;
    float zscale;
    uint flags;
    float alpha_ref;
    float pad0, pad1, pad2;
    vec4 fog_color;
} pc;

// flags
const uint F_TEXTURE = 1u << 0;
const uint F_OFFSET = 1u << 1;
const uint F_USE_ALPHA = 1u << 2;
const uint F_IGNORE_TEX_ALPHA = 1u << 3;
const uint F_ALPHA_TEST = 1u << 4;
const uint F_FOG_VERTEX = 1u << 5;
const uint F_TEXEL_BOX = 1u << 6;
const uint F_SHADE_SHIFT = 8u;     // 2 bits: decal, modulate, decal alpha, modulate alpha

void main() {
    vec4 base = v_base;
    if ((pc.flags & F_USE_ALPHA) == 0u) base.a = 1.0;
    vec4 c = base;
    if ((pc.flags & F_TEXTURE) != 0u) {
        vec2 uv = v_uv;
        if ((pc.flags & F_TEXEL_BOX) != 0u) {
            // a point-sampled polygon takes its own texels: those its box covers more than an eighth of. The game's
            // corners are a hair past whole texels (vk_renderer.c, uv_boxes), which a pixel centre can land in at a
            // fractional scale; at whole scales none does, and the texel each pixel takes stays the same
            vec2 size = vec2(textureSize(tex, 0));
            vec2 lo = floor(v_uvbox.xy * size + 0.125);
            vec2 hi = max(lo + 1.0, ceil(v_uvbox.zw * size - 0.125));
            uv = clamp(v_uv * size, lo, hi - 1.0 / 256.0) / size;
        }
        vec4 t = texture(tex, uv);
        if ((pc.flags & F_IGNORE_TEX_ALPHA) != 0u) t.a = 1.0;
        uint shade = (pc.flags >> F_SHADE_SHIFT) & 3u;
        if (shade == 0u) {
            c = t;
        } else if (shade == 1u) {
            c = vec4(t.rgb * base.rgb, t.a);
        } else if (shade == 2u) {
            c = vec4(mix(base.rgb, t.rgb, t.a), base.a);
        } else {
            c = t * base;
        }
        if ((pc.flags & F_OFFSET) != 0u) c.rgb += v_offset.rgb;
    }
    if ((pc.flags & F_FOG_VERTEX) != 0u) c.rgb = mix(c.rgb, pc.fog_color.rgb, v_offset.a);
    c = clamp(c, 0.0, 1.0);
    if ((pc.flags & F_ALPHA_TEST) != 0u && c.a < pc.alpha_ref) discard;
    o_color = c;
}
