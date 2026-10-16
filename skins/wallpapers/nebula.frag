// Shadertoy-style wallpaper: slow drifting nebula clouds with stars.
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.y;
    float t = iTime * 0.03;
    float n = 0.0, a = 0.5;
    vec2 p = uv * 2.2 + vec2(t, -t * 0.6);
    for (int i = 0; i < 4; i++) { n += a * noise(p); p = p * 2.0 + 3.1; a *= 0.5; }
    vec3 col = mix(vec3(0.02, 0.02, 0.08), vec3(0.45, 0.12, 0.55), n);
    col += vec3(0.1, 0.35, 0.6) * smoothstep(0.55, 1.0, n) * 0.6;
    float s = step(0.9965, hash(floor(fragCoord * 0.5)));
    col += s * (0.6 + 0.4 * sin(iTime + hash(floor(fragCoord)) * 6.0));
    fragColor = vec4(col, 1.0);
}
