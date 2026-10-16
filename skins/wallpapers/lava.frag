// Shadertoy-style wallpaper: slow lava-lamp metaballs.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.25;
    float f = 0.0;
    for (int i = 0; i < 5; i++) {
        float k = float(i);
        vec2 c = vec2(sin(t * (0.7 + k * 0.23) + k * 2.0) * 0.55,
                      cos(t * (0.5 + k * 0.31) + k * 1.3) * 0.32);
        f += 0.018 / (dot(uv - c, uv - c) + 0.004);
    }
    vec3 bg = mix(vec3(0.10, 0.02, 0.18), vec3(0.02, 0.02, 0.10), fragCoord.y / iResolution.y);
    vec3 blob = mix(vec3(1.0, 0.35, 0.25), vec3(1.0, 0.8, 0.35), clamp(f - 1.0, 0.0, 1.0));
    fragColor = vec4(mix(bg, blob * 0.85, smoothstep(0.95, 1.15, f)), 1.0);
}
