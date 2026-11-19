#include "vsm_address.sh"

bool sample_vsm_visibility(vec3 position, vec3 world_dx, vec3 world_dy, out float visibility)
{
    visibility = 1.0;
    if (u_vsm_layout.y < 0.5) return false;
    vec2 coordinate;
    uint address;
    vec4 grid;
    float camera_depth;
    if (!vsm_address(position, coordinate, address, grid, camera_depth)) return false;
    uint entry = vsm_entry(address);
    if ((entry & vsm_page_resident) == 0u) return false;
    vec4 depth = u_vsm_view[3];
    float inv_size = 1.0 / u_vsm_view[0].w;
    vec2 local = fract(coordinate);
    if (grid.w < 0.0) local.y = 1.0 - local.y;
    vec2 tile = vsm_atlas_origin(entry);
    vec4 rectangle = vec4((tile + vsm_page_border) * inv_size,
                          vec2_splat(vsm_page_size * inv_size));
    vec3 projected = vec3(local, (depth.x - dot(u_vsm_view[2].xyz, position)) * depth.y);
    if (projected.z < 0.0 || projected.z > 1.0) return false;
    vec3 dx = vec3(dot(u_vsm_view[0].xyz, world_dx) / grid.z,
                   dot(u_vsm_view[1].xyz, world_dx) / grid.z * grid.w,
                   -dot(u_vsm_view[2].xyz, world_dx) * depth.y);
    vec3 dy = vec3(dot(u_vsm_view[0].xyz, world_dy) / grid.z,
                   dot(u_vsm_view[1].xyz, world_dy) / grid.z * grid.w,
                   -dot(u_vsm_view[2].xyz, world_dy) * depth.y);
    float filtered = shadow_filter(projected, dx, dy, rectangle, inv_size,
                                   u_shadow_settings.x, vsm_page_border, true);
    float fade = smoothstep(u_vsm_view[5].w * 0.9, u_vsm_view[5].w, camera_depth);
    visibility = mix(filtered, 1.0, fade);
    return true;
}
vec3 vsm_debug_surface(vec3 color, vec3 position)
{
    if (u_vsm_layout.y < 0.5 || u_vsm_layout.z < 4.5) return color;
    vec2 coordinate;
    uint address;
    vec4 grid;
    float camera_depth;
    if (!vsm_address(position, coordinate, address, grid, camera_depth)) return color;
    uint entry = vsm_entry(address);
    if ((entry & vsm_page_dirty) != 0u) return vec3(1.0, 0.15, 0.12);
    if ((entry & vsm_page_resident) == 0u) return vec3(0.7, 0.2, 0.9);
    if ((entry & vsm_page_planned) != 0u) return vec3(1.0, 0.7, 0.12);
    return vec3(0.15, 0.65, 0.35);
}
