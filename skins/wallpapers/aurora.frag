// Shadertoy-style wallpaper: soft aurora curtains over a dark sky.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime * 0.12;
    vec3 col = mix(vec3(0.01, 0.02, 0.07), vec3(0.03, 0.05, 0.14), uv.y);
    for (int i = 0; i < 3; i++) {
        float f = float(i);
        float w = sin(uv.x * (3.0 + f) + t * (1.0 + f * 0.4) + f * 2.0) * 0.08
                + sin(uv.x * 7.0 - t * 1.7 + f) * 0.03;
        float d = uv.y - (0.55 + f * 0.09 + w);
        float band = exp(-d * d * 60.0) * smoothstep(-0.25, 0.1, d + 0.2);
        vec3 c = 0.5 + 0.5 * cos(vec3(0.0, 1.6, 3.2) + f * 1.3 + uv.x * 2.0 + t);
        col += band * c * vec3(0.35, 0.9, 0.7) * 0.45;
    }
    fragColor = vec4(col, 1.0);
}
