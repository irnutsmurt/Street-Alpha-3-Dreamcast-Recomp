#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 rect;
    vec4 params;  // scanline strength (0 = off), scanline count, mode (0 game, 1 overlay), picture height in pixels
    vec4 color;   // gamma exponent, saturation, CRT style (1 scanlines, 2 PVM), TV levels (1 = on)
} pc;

// the COLOR setting's response: TV/video levels (16-235 shown as 0-255, as a
// television or video capture displays the Dreamcast's output), then gamma
// and saturation
vec3 adjust(vec3 rgb) {
    if (pc.color.w > 0.5) rgb = clamp((rgb - 16.0 / 255.0) * (255.0 / 219.0), 0.0, 1.0);
    rgb = pow(rgb, vec3(pc.color.x));
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    return clamp(mix(vec3(luma), rgb, pc.color.y), 0.0, 1.0);
}

// The source, adjusted, in linear light, softened horizontally like a CRT beam.
vec3 beam_sample(vec2 uv) {
    float px = 1.0 / 640.0;
    vec3 c = adjust(texture(src, uv).rgb) * 0.5
           + adjust(texture(src, uv - vec2(px * 0.5, 0.0)).rgb) * 0.25
           + adjust(texture(src, uv + vec2(px * 0.5, 0.0)).rgb) * 0.25;
    return pow(c, vec3(2.4));
}

// A Trinitron-style PVM: 240 lines whose beam widens with brightness (bright
// lines fill the gaps, dark ones stay thin), an aperture grille mask at the
// screen's pixel pitch, and a little glow. s (0.1-1) scales all of it.
vec3 pvm(float s) {
    float lines = pc.params.y;
    float y = v_uv.y * lines - 0.5;
    float n = floor(y), f = y - n;
    vec3 a = beam_sample(vec2(v_uv.x, (n + 0.5) / lines));
    vec3 b = beam_sample(vec2(v_uv.x, (n + 1.5) / lines));
    // beam profile per channel: sigma in lines, from thin (dark) to wide (bright)
    float lo = mix(0.30, 0.13, s), hi = mix(0.45, 0.32, s);
    vec3 sa = mix(vec3(lo), vec3(hi), sqrt(a)), sb = mix(vec3(lo), vec3(hi), sqrt(b));
    vec3 wa = exp(-0.5 * (f * f) / (sa * sa)), wb = exp(-0.5 * ((1.0 - f) * (1.0 - f)) / (sb * sb));
    // a mid-width beam keeps its level; thin dark lines are lifted a little so
    // the picture doesn't sink at high strength
    vec3 scan = (a * wa + b * wb) / (0.5 * (lo + hi) * 2.5066) * (1.0 + 0.3 * s);
    vec3 flat_ = mix(a, b, f);
    vec3 rgb = mix(flat_, scan, s);
    // aperture grille: 3 pixels per R/G/B triad at 1300+ lines, magenta/green
    // pairs at 600+, none below (too coarse)
    float h = pc.params.w;
    int pitch = h >= 1300.0 ? 3 : h >= 600.0 ? 2 : 0;
    if (pitch > 0) {
        float dark = 1.0 - 0.55 * s;
        int i = int(gl_FragCoord.x) % pitch;
        vec3 m = pitch == 3 ? vec3(i == 0 ? 1.0 : dark, i == 1 ? 1.0 : dark, i == 2 ? 1.0 : dark)
                            : (i == 0 ? vec3(1.0, dark, 1.0) : vec3(dark, 1.0, dark));
        float avg = (1.0 + float(pitch - 1) * dark) / float(pitch);
        rgb *= m / avg;  // keep overall brightness
    }
    // glow: light spreading in the glass around bright areas
    vec2 r = vec2(2.5 / 640.0, 2.5 / 480.0);
    vec3 glow = (beam_sample(v_uv + vec2(r.x, 0.0)) + beam_sample(v_uv - vec2(r.x, 0.0)) +
                 beam_sample(v_uv + vec2(0.0, r.y)) + beam_sample(v_uv - vec2(0.0, r.y))) * 0.25;
    rgb += glow * glow * 0.15 * s;  // mostly from bright areas
    return pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / 2.4));
}

void main() {
    vec4 c = texture(src, v_uv);
    if (pc.params.z > 0.5) {  // menu overlay: straight alpha, blended by the pipeline
        o_color = c;
        return;
    }
    float s = pc.params.x;
    if (s > 0.0 && pc.color.z > 1.5) {
        o_color = vec4(pvm(s), 1.0);
        return;
    }
    vec3 rgb = adjust(c.rgb);
    if (s > 0.0) {
        // Dark gap at the bottom of each source line, at least one screen pixel
        // (or 30% of the line) thick, with half-pixel soft edges. Defined in
        // screen pixels so it can't alias away at 2 pixels per line.
        float k = pc.params.w / pc.params.y;          // screen pixels per scanline
        float y = v_uv.y * pc.params.y;
        float t = max(1.0 / k, 0.3);                  // gap thickness, in lines
        float e = 0.5 / k;                            // half a pixel, in lines
        float gap = smoothstep(1.0 - t - e, 1.0 - t + e, fract(y));
        rgb *= (1.0 - s * gap) / (1.0 - s * t * 0.75);  // keep overall brightness
    }
    o_color = vec4(min(rgb, vec3(1.0)), 1.0);
}
