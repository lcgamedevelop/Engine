/*
 * PCF and projected local lights adapt bgfx 15-shadowmaps-simple
 * and 16-shadowmaps at 7a70927d2af7ffcc5cef9c9e8f4c821e31fcb099.
 * Copyright 2013-2014 Dario Manesku, BSD-2-Clause; see notices/bgfx.txt.
 * Point lights use six ordinary 90-degree faces in the existing 2D atlas;
 * this is not the example's optional tetrahedral/stencil packing.
 */
#ifndef ENGINE_LOCAL_SHADOW_HELPERS_SH
#define ENGINE_LOCAL_SHADOW_HELPERS_SH

// Include cluster/lighting.sh and csm_helpers.sh before this file.

vec4 local_shadow_project(uint record, vec3 position)
{
    int base = int(u_cluster_counts.z) + int(record) * 6;
    // Matrices are uploaded as four columns; this avoids backend constructor layout assumptions.
    return b_light_data[base] * position.x + b_light_data[base + 1] * position.y +
           b_light_data[base + 2] * position.z + b_light_data[base + 3];
}

uint point_shadow_face(vec3 direction)
{
    vec3 axis = abs(direction);
    if (axis.x >= axis.y && axis.x >= axis.z) return direction.x >= 0.0 ? 0u : 1u;
    if (axis.y >= axis.z) return direction.y >= 0.0 ? 2u : 3u;
    return direction.z >= 0.0 ? 4u : 5u;
}

vec3 point_shadow_ray(uint face, vec2 ndc)
{
    vec3 forward = vec3(1.0, 0.0, 0.0);
    vec3 up = vec3(0.0, -1.0, 0.0);
    if (face == 1u) forward = vec3(-1.0, 0.0, 0.0);
    if (face == 2u) { forward = vec3(0.0, 1.0, 0.0); up = vec3(0.0, 0.0, 1.0); }
    if (face == 3u) { forward = vec3(0.0, -1.0, 0.0); up = vec3(0.0, 0.0, -1.0); }
    if (face == 4u) forward = vec3(0.0, 0.0, 1.0);
    if (face == 5u) forward = vec3(0.0, 0.0, -1.0);
    if (u_cluster_camera.w < 0.5) ndc.y = -ndc.y;
    vec3 right = cross(forward, up);
    return normalize(forward + right * ndc.x + up * ndc.y);
}

float local_shadow_visibility(ClusterLightData light, vec3 world_position,
                              vec3 receiver_offset, float radial_distance)
{
    if (light.kind_inner.z < 0.5) return 1.0;
    uint first = uint(light.kind_inner.z) - 1u;
    bool is_point = light.kind_inner.w > 1.5;
    uint face = is_point ? point_shadow_face(receiver_offset) : 0u;
    uint record = first + face;
    vec4 position = local_shadow_project(record, world_position);
    if (position.w <= 0.0) return 1.0;
    vec3 projected = position.xyz / position.w;
    if (any(lessThan(projected, vec3_splat(0.0))) ||
        any(greaterThan(projected, vec3_splat(1.0)))) return 1.0;
    int base = int(u_cluster_counts.z) + int(record) * 6;
    vec4 rectangle = b_light_data[base + 4];
    vec4 parameters = b_light_data[base + 5];
    vec2 texel_step = parameters.y / rectangle.zw;
    vec2 half_texel = vec2_splat(parameters.y * 0.5);
    float depth_scale = 0.0;
    float depth_offset = 0.0;
    if (is_point)
    {
        depth_scale = parameters.z;
        depth_offset = parameters.w / radial_distance;
    }
    float visible = 0.0;
    for (int x = 0; x < 4; ++x)
    {
        for (int y = 0; y < 4; ++y)
        {
            vec2 local_uv = projected.xy + (vec2(float(x), float(y)) - 1.5) * texel_step;
            vec4 sample_rectangle = rectangle;
            float receiver_depth = projected.z;
            if (is_point)
            {
                vec2 ndc = local_uv * 2.0 - 1.0;
                if (all(lessThan(abs(ndc), vec2_splat(1.0))))
                {
                    // An interior 90-degree ray has unit forward distance before
                    // normalization. Project the same radial distance directly.
                    receiver_depth = depth_scale - depth_offset * sqrt(1.0 + dot(ndc, ndc));
                }
                else
                {
                    // Cube edges need the adjacent face's UV, depth and tile.
                    vec3 ray = point_shadow_ray(face, ndc);
                    uint sample_face = point_shadow_face(ray);
                    vec3 receiver = light.position_range.xyz + ray * radial_distance;
                    vec4 p = local_shadow_project(first + sample_face, receiver);
                    local_uv = p.xy / p.w;
                    receiver_depth = p.z / p.w;
                    int sample_base = int(u_cluster_counts.z) + int(first + sample_face) * 6;
                    sample_rectangle = b_light_data[sample_base + 4];
                }
            }
            vec2 uv = sample_rectangle.xy + local_uv * sample_rectangle.zw;
            uv = clamp(uv, sample_rectangle.xy + half_texel,
                        sample_rectangle.xy + sample_rectangle.zw - half_texel);
            float sample_depth = texture2DLod(s_shadow_map, uv, 0.0).r;
            visible += step(receiver_depth - parameters.x, sample_depth);
        }
    }
    return visible / 16.0;
}

#endif
