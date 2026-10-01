// Jarzynski and Olano's pcg4d (2020): four 32-bit hashes of four 32-bit inputs, mixed together.
// Integer rather than the sin-fract form noise.glsl carries, because its callers' seeds are
// consecutive integers -- drop indices, ripple cells -- which is the regime a sin-fract hash
// lines up in. Divide by 4294967296.0 for [0,1).
uvec4 pcg4d(uvec4 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    return v;
}
