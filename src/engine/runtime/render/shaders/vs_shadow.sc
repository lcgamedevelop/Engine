$input a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0

// bgfx 15-shadowmaps-simple, BSD-2-Clause. See third_party/notices/bgfx.txt.
#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
}
