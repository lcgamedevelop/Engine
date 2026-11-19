$input v_world_position, v_world_normal, v_world_tangent, v_texcoord0, v_color0, v_lightmap_uv

#include "pbr_surface.sh"

void main()
{
    if (u_material.w > 0.0)
    {
        vec2 uv = v_texcoord0 * u_uv_transform.xy + u_uv_transform.zw;
        vec4 base_texture;
        if (u_hex_tiling.x > 0.5)
        {
            hextile_context(uv, u_hex_tiling.y, u_hex_tiling.z, s_base_color, base_texture);
        }
        else
        {
            base_texture = texture2D(s_base_color, uv);
        }
        vec4 base_sample = base_texture * u_base_color;
        vec4 base_color = base_sample * v_color0;
        apply_alpha_cutoff(base_color.a, u_material.w);
    }
    gl_FragColor = vec4_splat(0.0);
}
