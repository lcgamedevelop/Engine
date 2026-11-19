#ifndef ENGINE_VSM_ADDRESS_SH
#define ENGINE_VSM_ADDRESS_SH

// Must match vsm_pack_view. The first three vectors contain the light basis,
// with the atlas size, finest texel size, and dominant debug level in W.
uniform vec4 u_vsm_view[22];
const float vsm_page_size = 128.0;
const float vsm_page_border = 4.0;
const int vsm_first_clipmap = 6;

#if !defined(ENGINE_VSM_FEEDBACK)
// Only the receiver's page entry varies by address in the clustered light buffer.
uniform vec4 u_vsm_layout;

const uint vsm_origin_bits = 14u;
const uint vsm_origin_mask = (1u << vsm_origin_bits) - 1u;
const uint vsm_page_resident = 1u << 28u;
const uint vsm_page_planned = 1u << 29u;
const uint vsm_page_dirty = 1u << 30u;

uint vsm_entry(uint address)
{
    vec4 values = b_light_data[int(u_vsm_layout.x) + int(address / 4u)];
    return floatBitsToUint(values[int(address % 4u)]);
}

vec2 vsm_atlas_origin(uint entry)
{
    return vec2(float(entry & vsm_origin_mask),
                float((entry >> vsm_origin_bits) & vsm_origin_mask));
}
#endif

bool vsm_address(vec3 position, out vec2 coordinate, out uint address,
                 out vec4 grid, out float camera_depth)
{
    vec4 counts = u_vsm_view[3];
    vec4 camera = u_vsm_view[4];
    vec4 options = u_vsm_view[5];
    camera_depth = dot(camera.xyz, position);
    coordinate = vec2_splat(0.0);
    address = 0u;
    grid = vec4_splat(0.0);
    if (camera_depth < camera.w || camera_depth >= options.w) return false;
    float footprint = options.x * (options.y > 0.5 ? 1.0 : max(camera_depth, camera.w));
    int level = int(clamp(floor(log2(max(footprint / u_vsm_view[1].w, 0.00001)) + options.z),
                          0.0, counts.w - 1.0));
    vec2 light = vec2(dot(u_vsm_view[0].xyz, position), dot(u_vsm_view[1].xyz, position));
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        grid = u_vsm_view[vsm_first_clipmap + level];
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
