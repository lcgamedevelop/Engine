#ifndef ENGINE_VSM_ADDRESS_SH
#define ENGINE_VSM_ADDRESS_SH

// The table shares the clustered light buffer. Address math is identical in demand and shading.
uniform vec4 u_vsm_layout;
vec4 vsm_data(int offset) { return b_light_data[int(u_vsm_layout.x) + offset]; }

bool vsm_address(vec3 position, out int level, out vec2 coordinate, out uint address)
{
    vec4 counts = vsm_data(3);
    vec4 camera = vsm_data(6);
    vec4 options = vsm_data(7);
    float distance = dot(camera.xyz, position);
    level = 0;
    coordinate = vec2_splat(0.0);
    address = 0u;
    if (distance < camera.w || distance >= options.w) return false;
    float footprint = options.x * (options.y > 0.5 ? 1.0 : max(distance, camera.w));
    level = int(clamp(floor(log2(max(footprint / vsm_data(5).z, 0.00001)) + options.z),
                       0.0, counts.w - 1.0));
    vec2 light = vec2(dot(vsm_data(0).xyz, position), dot(vsm_data(1).xyz, position));
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        vec4 grid = vsm_data(8 + level);
        coordinate = (light - grid.xy) / grid.z;
        if (all(greaterThanEqual(coordinate, vec2_splat(0.0))) &&
            all(lessThan(coordinate, vec2_splat(counts.z))))
        {
            address = uint(level) * uint(counts.z * counts.z) +
                      uint(coordinate.y) * uint(counts.z) + uint(coordinate.x);
            return true;
        }
        ++level;
        if (level >= int(counts.w)) return false;
    }
    return false;
}
#endif
