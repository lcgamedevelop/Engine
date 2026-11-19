$input v_texcoord0, v_color0

#include <bgfx_shader.sh>
#include "material_helpers.sh"

// x: alpha cutoff, y: base color alpha.
uniform vec4 u_shadow_alpha;
uniform vec4 u_shadow_uv;
SAMPLER2D(s_shadow_alpha, 0);

void main()
{
    if (u_shadow_alpha.x > 0.0)
    {
        vec2 uv = v_texcoord0 * u_shadow_uv.xy + u_shadow_uv.zw;
        float alpha = texture2D(s_shadow_alpha, uv).a * u_shadow_alpha.y * v_color0.a;
        apply_alpha_cutoff(alpha, u_shadow_alpha.x);
    }
}
