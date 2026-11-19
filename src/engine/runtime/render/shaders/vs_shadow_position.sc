$input a_position
$output v_texcoord0, v_color0

// Position transform adapted from bgfx 15-shadowmaps-simple, see third_party/notices/bgfx.txt.
#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = vec2_splat(0.0);
    v_color0 = vec4_splat(1.0);
}
