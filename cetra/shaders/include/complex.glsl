// Complex arithmetic on vec2 (real, imaginary), for the passes that transform by FFT.

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complexConj(vec2 a) {
    return vec2(a.x, -a.y);
}
