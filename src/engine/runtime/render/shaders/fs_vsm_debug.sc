$input v_texcoord0
#include <bgfx_shader.sh>
#include "cluster/lighting.sh"
#include "vsm/vsm_address.sh"
SAMPLER2D(s_virtual_shadow, 10);
uniform vec4 u_vsm_debug;
void main()
{
    if (u_vsm_debug.x < 0.0)
    {
        gl_FragColor = vec4(0.025, 0.035, 0.06, 0.92);
        return;
    }
    vec2 uv = clamp(v_texcoord0, vec2_splat(0.000001), vec2_splat(0.999999));
    vec3 color = vec3(0.7, 0.2, 0.9);
    if (u_vsm_debug.x > 0.5 && u_vsm_debug.x < 1.5)
    {
        float depth = texture2DLod(s_virtual_shadow, uv, 0.0).r;
        color = vec3_splat(depth);
    }
    else
    {
        int level = int(clamp(u_vsm_debug.z < 0.0 ? u_vsm_view[2].w : u_vsm_debug.z,
                              0.0, u_vsm_view[3].w - 1.0));
        float n = u_vsm_view[3].z;
        uv.y = 1.0 - uv.y;
        uint address = uint(level) * uint(n * n) + uint(uv.y * n) * uint(n) + uint(uv.x * n);
        uint entry = vsm_entry(address);
        if ((entry & vsm_page_dirty) != 0u) color = vec3(1.0, 0.15, 0.12);
        if ((entry & vsm_page_resident) != 0u)
        {
            if (u_vsm_debug.x > 1.5)
                color = (entry & vsm_page_planned) != 0u ? vec3(1.0, 0.7, 0.12)
                                                        : vec3(0.15, 0.65, 0.35);
            else
            {
                vec2 local = fract(uv * n);
                vec2 tile = vsm_atlas_origin(entry);
                if (u_vsm_view[vsm_first_clipmap + level].w < 0.0) local.y = 1.0 - local.y;
                vec2 sample_uv = (tile + vsm_page_border + local * vsm_page_size) / u_vsm_view[0].w;
                color = vec3_splat(texture2DLod(s_virtual_shadow, sample_uv, 0.0).r);
            }
        }
    }
    gl_FragColor = vec4(color, 1.0);
}
