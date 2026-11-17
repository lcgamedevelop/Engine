#include "vsm_address.sh"

uint vsm_entry(uint address)
{
    vec4 values = vsm_data(24 + int(address / 4u));
    return floatBitsToUint(values[int(address % 4u)]);
}
float vsm_visibility(vec3 position, vec3 world_dx, vec3 world_dy, float fallback)
{
    if (u_vsm_layout.y < 0.5) return fallback;
    int level;
    vec2 coordinate;
    uint address;
    if (!vsm_address(position, level, coordinate, address)) return fallback;
    uint entry = vsm_entry(address);
    uint slot = entry & 65535u;
    if (slot == 0u) return fallback;
    --slot;
    vec4 atlas = vsm_data(4);
    vec4 grid = vsm_data(8 + level);
    vec4 depth = vsm_data(3);
    float inv_size = 1.0 / vsm_data(5).x;
    vec2 local = fract(coordinate);
    if (grid.w < 0.0) local.y = 1.0 - local.y;
    vec2 tile = vec2(float(slot % uint(atlas.w)), float(slot / uint(atlas.w))) * atlas.z;
    if (grid.w > 0.0) tile.y = vsm_data(5).x - tile.y - atlas.z;
    vec4 rectangle = vec4((tile + atlas.y) * inv_size, vec2_splat(atlas.x * inv_size));
    vec3 projected = vec3(local, (depth.x - dot(vsm_data(2).xyz, position)) * depth.y);
    if (projected.z < 0.0 || projected.z > 1.0) return fallback;
    vec3 dx = vec3(dot(vsm_data(0).xyz, world_dx) / grid.z,
                   dot(vsm_data(1).xyz, world_dx) / grid.z * grid.w,
                   -dot(vsm_data(2).xyz, world_dx) * depth.y);
    vec3 dy = vec3(dot(vsm_data(0).xyz, world_dy) / grid.z,
                   dot(vsm_data(1).xyz, world_dy) / grid.z * grid.w,
                   -dot(vsm_data(2).xyz, world_dy) * depth.y);
    float visibility = shadow_filter(projected, dx, dy, rectangle, inv_size,
                                      u_shadow_settings.x, atlas.y, true);
    float camera_depth = dot(vsm_data(6).xyz, position);
    float fade = smoothstep(vsm_data(7).w * 0.9, vsm_data(7).w, camera_depth);
    return mix(visibility, 1.0, fade);
}
vec3 vsm_debug_surface(vec3 color, vec3 position)
{
    if (u_vsm_layout.y < 0.5 || u_vsm_layout.z < 4.5) return color;
    int level;
    vec2 coordinate;
    uint address;
    if (!vsm_address(position, level, coordinate, address)) return color;
    uint entry = vsm_entry(address);
    if ((entry & 131072u) != 0u) return vec3(1.0, 0.15, 0.12);
    if ((entry & 65535u) == 0u) return vec3(0.7, 0.2, 0.9);
    if ((entry & 65536u) != 0u) return vec3(1.0, 0.7, 0.12);
    return vec3(0.15, 0.65, 0.35);
}
