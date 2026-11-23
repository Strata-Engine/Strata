// The lit image, to the screen.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 10) uniform sampler2D lit;
layout(location = 0) out vec4 o_color;

// ── Rain over the view (sv_set_weather, sv_set_rain) ────────────────────
float rain_hash(float x) { return fract(sin(x * 12.9898) * 43758.5453); }

// The screen direction rain falls (unit, pixels, y down): the world direction
// u.rain.xyz as the camera sees it about 40 m ahead. Rain falling straight at
// the eye is drawn falling down the screen.
vec2 rain_screen_dir(vec2 size) {
    vec3 ahead = normalize(world_at(vec2(0.5), 1.0) - u.eye.xyz);
    vec3 p0 = u.eye.xyz + ahead * 40.0;
    vec4 a = u.vp * vec4(p0, 1.0), b = u.vp * vec4(p0 + u.rain.xyz * 4.0, 1.0);
    vec2 d = (b.xy / b.w - a.xy / a.w) * vec2(0.5, -0.5) * size;
    float l = length(d);
    return l < 1.0e-3 ? vec2(0.0, 1.0) : mix(vec2(0.0, 1.0), d / l, smoothstep(2.0, 12.0, l));
}

// One depth of streaks at pixel px: columns [width] pixels wide along [dir],
// [density] of the slots holding a drop, falling [speed] slots a second.
float rain_layer(vec2 px, vec2 dir, float width, float speed, float density, float seed) {
    vec2 q = vec2(dot(px, vec2(-dir.y, dir.x)), dot(px, dir)) / width;
    float column = floor(q.x);
    float h = rain_hash(column + seed);
    float along = q.y * 0.07 - u.rain.w * speed * (0.8 + 0.4 * h) + h * 41.0;
    float slot = floor(along), f = fract(along);
    float h2 = rain_hash(column * 7.31 + slot * 1.73 + seed);
    if (h2 > density) return 0.0;
    float len = 0.22 + 0.3 * rain_hash(h2 * 91.0);
    float streak = smoothstep(0.0, 0.04, f) * (1.0 - smoothstep(len * 0.5, len, f));
    float across = abs(fract(q.x) - 0.5 - (h - 0.5) * 0.6);
    return streak * (1.0 - smoothstep(0.03, 0.14, across));
}

vec3 rained(vec3 c, vec2 px, vec2 size) {
    float rain = u.weather.w;
    if (rain <= 0.0) return c;
    vec2 dir = rain_screen_dir(size);
    float scale = size.y / 900.0;                     // keep the streaks' size steady across resolutions
    float s = 0.30 * rain_layer(px, dir, 17.0 * scale, 5.5, 0.10 + 0.25 * rain, 3.0)
            + 0.22 * rain_layer(px, dir, 9.0 * scale, 4.0, 0.12 + 0.30 * rain, 11.0)
            + 0.15 * rain_layer(px, dir, 5.0 * scale, 3.0, 0.15 + 0.35 * rain, 23.0);
    float day = smoothstep(-0.15, 0.25, u.sun.y);
    vec3 drop = vec3(0.70, 0.75, 0.82) * (0.05 + 0.68 * day) + vec3(0.8, 0.85, 1.0) * u.weather.z;
    c = mix(c, drop * 0.5, 0.07 * rain);              // the haze of rain too fine to see
    return mix(c, drop, clamp(s * (0.4 + 0.6 * rain), 0.0, 1.0));
}

void main() {
    // under water the whole view sways a little
    vec2 uv = v_uv;
    bool lit_mode = u.light.x >= 1.0 && u.light.x < 9.0;
    if (u.water.w > 0.0 && lit_mode) uv = clamp(uv + 0.0035 * vec2(sin(uv.y * 31.0 + u.light.y * 1.9), sin(uv.x * 27.0 + u.light.y * 1.5)), 0.001, 0.999);
    vec3 c = texture(lit, uv).rgb;
    if (lit_mode && u.water.w <= 0.0) c = rained(c, gl_FragCoord.xy, vec2(textureSize(lit, 0)));
    o_color = vec4(lit_mode ? tonemap(c) : c, 1.0);
}
