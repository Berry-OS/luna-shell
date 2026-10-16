// Shadertoy-style wallpaper: layered ocean waves at dusk.
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime * 0.3;
    vec3 col = mix(vec3(0.98, 0.55, 0.35), vec3(0.10, 0.12, 0.38), uv.y);
    for (int i = 0; i < 5; i++) {
        float f = float(4 - i);   // far layer first, near layer last
        float y = 0.12 + f * 0.1 + sin(uv.x * (2.5 + f * 0.7) + t * (0.6 + f * 0.2) + f) * 0.035;
        float m = smoothstep(y + 0.004, y - 0.004, uv.y);
        vec3 c = mix(vec3(0.20, 0.45, 0.65), vec3(0.03, 0.08, 0.22), 1.0 - f / 4.0);
        col = mix(col, c, m);
    }
    fragColor = vec4(col * 0.85, 1.0);
}
