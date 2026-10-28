#version 450
// 16:9 background extension (vk_renderer.c, widescreen.c): a background layer's fill where it lacks art, worked out
// once a stage from the layer's own art (wide_filler.c's wide_extend) in its map (a unit by two guest pixels: its
// texels' size), here placed where the layer is this frame. Drawn before the frame's polygons, writing depth:
//  - a gap (mode 128: past the art's end, the backdrop's holes) just behind the layer: whatever the layer draws,
//    drawn after, covers it to the pixel (its translucent tiles' edges blend over it), farther layers don't (the
//    game draws a layer's fully opaque tiles in the opaque list, those with transparent texels in the others);
//  - the art next to a gap (mode 64), behind it too: the layer is drawn up to a pixel off the map's grid, and this
//    closes the seam;
//  - its filler (mode 255): just in front of the layer, under everything nearer.
// The colours are premultiplied (alpha 1 where filled): bilinear between the fill's texels without the unfilled
// ones darkening the edges. They follow the palette of the art each comes from as it changes (a stage's palette
// animation: the palette's colour now against the capture's, vk_renderer.c's pal_table), and are shaded as the
// layer's polygons are this frame (a super darkens the stage by their colour).
layout(set = 0, binding = 0) uniform sampler2D fill;
layout(set = 1, binding = 0) uniform sampler2D modes;  // the mode, the palette (0 none)
layout(set = 2, binding = 0) uniform sampler2D pals;   // a layer's palettes: times (row 2 x layer), plus (the next)
layout(push_constant) uniform PC {
    vec4 map;    // the map column at the picture's left, columns per pixel, the row at its top, rows per pixel
    vec4 layer;  // its first row in the images, its depth [0, 1], 1 = the backdrop (farthest), mode (9: testing)
    vec4 quad;   // the columns (ext.vert); the layer's colour this frame (its texels times it), red, green
    vec4 shade;  // the colour's blue; the offset colour (added), red, green, blue
    vec4 rows;   // the rows (ext.vert)
} pc;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 p = gl_FragCoord.xy;
    float u = pc.map.x + p.x * pc.map.y, v = pc.map.z + p.y * pc.map.w;
    if (u < 0.0 || u >= 1024.0) discard;
    bool past = v < 0.0 || v >= 240.0;  // past the rows captured (the camera risen for a jump): the nearest row's
    v = clamp(v, 0.0, 239.99);
    ivec2 t = ivec2(int(u), int(pc.layer.x + v));
    vec2 mp = texelFetch(modes, t, 0).rg;
    float m = mp.r;
    if (past && pc.layer.z > 0.5) m = max(m, 0.5);  // the backdrop's: a gap (whatever it draws there covers it)
    if (m < 0.125) discard;
    bool over = m > 0.75;
    vec4 c = texture(fill, vec2(u, pc.layer.x + clamp(v, 0.5, 239.5)) / vec2(textureSize(fill, 0)));
    vec3 rgb = c.a > 0.01 ? c.rgb / c.a : texelFetch(fill, t, 0).rgb;
    int pal = int(mp.g * 255.0 + 0.5), row = 2 * (int(pc.layer.x + 0.5) / 240);
    if (pal > 0) rgb = rgb * texelFetch(pals, ivec2(pal - 1, row), 0).rgb + texelFetch(pals, ivec2(pal - 1, row + 1), 0).rgb;
    rgb = clamp(rgb * vec3(pc.quad.zw, pc.shade.x) + pc.shade.yzw, 0.0, 1.0);  // shaded as the layer is (pvr.frag)
    if (pc.layer.w > 8.5) rgb = over ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 1.0, 0.0);  // testing: what's filled
    gl_FragDepth = pc.layer.y * (over ? 1.0005 : 0.9995);
    o_color = vec4(rgb, 1.0);
}
