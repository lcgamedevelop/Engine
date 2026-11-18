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
        int level = int(clamp(u_vsm_debug.z < 0.0 ? vsm_data(5).w : u_vsm_debug.z,
                              0.0, vsm_data(3).w - 1.0));
        float n = vsm_data(3).z;
        uv.y = 1.0 - uv.y;
        uint address = uint(level) * uint(n * n) + uint(uv.y * n) * uint(n) + uint(uv.x * n);
        vec4 packed = vsm_data(24 + int(address / 4u));
        uint entry = floatBitsToUint(packed[int(address % 4u)]);
        uint slot = entry & 65535u;
        if ((entry & 131072u) != 0u) color = vec3(1.0, 0.15, 0.12);
        if (slot > 0u)
        {
            --slot;
            if (u_vsm_debug.x > 1.5)
                color = (entry & 65536u) != 0u ? vec3(1.0, 0.7, 0.12) : vec3(0.15, 0.65, 0.35);
            else
            {
                vec4 atlas = vsm_data(4);
                vec2 local = fract(uv * n);
                vec2 tile = vec2(float(slot % uint(atlas.w)), float(slot / uint(atlas.w))) * atlas.z;
                if (vsm_data(8 + level).w < 0.0) local.y = 1.0 - local.y;
                else tile.y = vsm_data(5).x - tile.y - atlas.z;
                vec2 sample_uv = (tile + atlas.y + local * atlas.x) / vsm_data(5).x;
                color = vec3_splat(texture2DLod(s_virtual_shadow, sample_uv, 0.0).r);
            }
        }
    }
    gl_FragColor = vec4(color, 1.0);
}
