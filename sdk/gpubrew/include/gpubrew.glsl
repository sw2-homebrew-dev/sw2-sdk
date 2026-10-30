// gpubrew.glsl - sw2-sdk interface for GPUBrew ABI 1 shader apps.
//
// Written from the published ABI 1 byte layout (8 MiB std430 buffer at set 0,
// binding 0; 1024-byte state header; app data from byte 1024).  Source
// compatible with apps that address the same offsets.
//
// Compute owns writes.  In vertex/fragment stages define GB_READONLY before
// including this file.
#ifndef GPUBREW_GLSL
#define GPUBREW_GLSL

#ifdef GB_READONLY
#define GB_ACCESS readonly
#else
#define GB_ACCESS
#endif

layout(set=0, binding=0, std430) GB_ACCESS buffer GpuBrewState {
    uvec4 raw[4];        //    0  pad: [0].x sample, [0].z buttons, [1] sticks, [2].x attributes
    vec4  motion;        //   64  app timing (motion.x: seconds by convention)
    uvec4 state;         //   80  state.y previous buttons, state.z frame count
    vec4  refresh;       //   96  menu bookkeeping - leave alone
    uvec4 library;       //  112  menu bookkeeping - leave alone
    uint  touch[164];    //  128  touch: [2] count, point i at [8+10i], [9+10i]
    uvec4 user0;         //  784  app-owned scratch: 176 bytes
    ivec4 user1;         //  800
    vec4  user2;         //  816
    uvec4 user3;         //  832
    uint  user4[8];      //  848
    uint  user5[20];     //  880
    vec4  transition;    //  960  x: fade opacity (1 -> 0 fades in)
    uvec4 events;        //  976  x: sound sequence, y: sound id
    uvec4 stats;         //  992  app-owned
    uvec4 reserved;      // 1008  z: exit request (keep set), others runtime-owned
    uint  data[];        // 1024  app data words
} gb;

const uint GB_A = 1u, GB_B = 2u, GB_X = 4u, GB_Y = 8u;
const uint GB_STICK_L = 16u, GB_STICK_R = 32u, GB_L = 64u, GB_R = 128u;
const uint GB_ZL = 256u, GB_ZR = 512u, GB_PLUS = 1024u, GB_MINUS = 2048u;
const uint GB_LEFT = 4096u, GB_UP = 8192u, GB_RIGHT = 16384u, GB_DOWN = 32768u;
const vec2 GB_SCREEN = vec2(1920.0, 1080.0);

uint gbButtons()   { return gb.raw[0].z; }
uint gbPressed()   { return gb.raw[0].z & ~gb.state.y; }
bool gbConnected() { return (gb.raw[2].x & 1u) != 0u; }
uint gbFrame()     { return gb.state.z; }

// -1..1 with a 15% radial deadzone; y is up.
vec2 gbStick(bool right) {
    ivec2 v = right ? ivec2(gb.raw[1].zw) : ivec2(gb.raw[1].xy);
    vec2 s = clamp(vec2(v) / 32767.0, -1.0, 1.0);
    float n = length(s);
    return n <= 0.15 ? vec2(0.0) : s / n * min(1.0, (n - 0.15) / 0.85);
}

uint gbTouchCount() { return min(gb.touch[2], 16u); }
// Touch in 1920x1080 screen pixels (the panel reports 1280x720).
vec2 gbTouch(uint i) {
    if (i >= gbTouchCount()) return vec2(-1.0);
    return vec2(float(gb.touch[8u + i * 10u]), float(gb.touch[9u + i * 10u])) * 1.5;
}

float gbDataFloat(uint word) { return uintBitsToFloat(gb.data[word]); }
uint  gbDataByte(uint index) { return (gb.data[index >> 2u] >> ((index & 3u) * 8u)) & 255u; }

#ifndef GB_READONLY
// Call once at the end of compute main().
void gbEndFrame() { gb.state.y = gb.raw[0].z; gb.state.z++; }
void gbExit()     { gb.reserved.z = 1u; }
void gbSound(uint id) { gb.events.x++; gb.events.y = id; }
#endif

// --- small drawing helpers (fragment) ---------------------------------
float gbBox(vec2 p, vec2 center, vec2 half_size, float radius) {
    vec2 q = abs(p - center) - half_size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}
float gbFill(float d) { return 1.0 - smoothstep(-0.8, 0.8, d); }
float gbOutline(float d, float width) { return 1.0 - smoothstep(width - 0.8, width + 0.8, abs(d)); }

#endif
