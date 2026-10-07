// The engine's display encode -- a plain 2.2 power, not the sRGB curve -- and its inverse. One
// statement, so a pass that undoes the encode and redoes it cannot disagree with the tonemap
// that first applied it.

// Gamma-encode a linear [0,1] color for display.
vec3 displayEncode(vec3 c)
{
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

// A display-encoded color back to linear.
vec3 displayDecode(vec3 c)
{
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}
