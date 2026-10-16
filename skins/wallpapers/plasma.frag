// Shadertoy-style wallpaper: copy to ~/.config/luna-shell/wallpapers/ and pick it.
// Available: iResolution (vec3), iTime (seconds), iFrame (int).
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / iResolution.xy;
    float t = iTime * 0.25;
    float v = sin(uv.x * 6.0 + t) + sin(uv.y * 5.0 - t * 1.3)
            + sin((uv.x + uv.y) * 4.0 + t * 0.7);
    vec3 col = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + v + t);
    fragColor = vec4(col * 0.45 + 0.05, 1.0);
}
