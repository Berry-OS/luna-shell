// Shadertoy-style wallpaper: calm flowing gradient (very cheap).
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime * 0.15;
    vec3 a = vec3(0.10, 0.12, 0.35);
    vec3 b = vec3(0.55, 0.25, 0.65);
    vec3 c = vec3(0.15, 0.55, 0.65);
    float w1 = 0.5 + 0.5 * sin(uv.x * 2.0 + uv.y * 1.5 + t);
    float w2 = 0.5 + 0.5 * sin(uv.y * 3.0 - uv.x * 1.0 - t * 1.3);
    vec3 col = mix(mix(a, b, w1), c, w2 * 0.6);
    fragColor = vec4(col * 0.8, 1.0);
}
