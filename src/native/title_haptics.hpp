#pragma once
#include <array>
#include <cmath>
namespace title_haptics {
// Dedicated title UI, stereo actuator samples at 48 kHz. No speaker channel.
inline float Duration(unsigned cue) {
    constexpr float lengths[]{0.058f,0.180f,0.120f,0.420f,0.280f};
    return cue < 5 ? lengths[cue] : 0.0f;
}
inline float Pulse(float t,float delay,float length,float hz,float gain) {
    t-=delay;
    if(t<=0.0f || t>=length) return 0.0f;
    constexpr float pi=3.14159265358979323846f;
    float window=std::sin(pi*t/length);
    return gain*window*window*std::sin(2.0f*pi*hz*t);
}
inline std::array<float,2> Sample(unsigned cue,float t) {
    if (t < 0.0f || t >= Duration(cue)) return {};
    auto p=[t](float d,float l,float f,float a){return Pulse(t,d,l,f,a);};
    switch(cue) {
    case 0: return {p(0,0.044f,125,0.20f)+p(0.020f,0.038f,190,0.075f),
                   p(0.006f,0.046f,170,0.22f)};
    case 1: return {p(0,0.055f,165,0.14f)+p(0.055f,0.125f,85,0.27f),
                   p(0.008f,0.055f,190,0.18f)+p(0.070f,0.105f,105,0.22f)};
    case 2: return {p(0.020f,0.100f,85,0.15f),p(0,0.065f,145,0.16f)};
    case 3: return {p(0,0.310f,70,0.20f)+p(0.260f,0.140f,115,0.12f),
                   p(0.055f,0.335f,90,0.20f)+p(0.300f,0.120f,150,0.10f)};
    case 4: return {p(0,0.070f,145,0.17f)+p(0.070f,0.210f,75,0.30f),
                   p(0.012f,0.080f,180,0.19f)+p(0.105f,0.170f,100,0.27f)};
    default: return {};
    }
}
}
