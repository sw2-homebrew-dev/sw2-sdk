#version 450
#extension GL_GOOGLE_include_directive : require
#define GB_READONLY
#include "gpubrew.glsl"
layout(location = 0) out vec4 color;

void main() {
    vec2 p = gl_FragCoord.xy;
    float t = float(gbFrame()) / 60.0;
    vec3 rgb = mix(vec3(0.05, 0.08, 0.14), vec3(0.10, 0.20, 0.32), p.y / GB_SCREEN.y);
    rgb = mix(rgb, vec3(0.95, 0.35, 0.25), gbFill(gbBox(p, gb.user2.xy, vec2(60.0), 18.0)));
    rgb += 0.04 * sin(t + p.x * 0.01);
    color = vec4(rgb * (1.0 - gb.transition.x), 1.0);
}
