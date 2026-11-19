$input v_world_position, v_world_normal, v_world_tangent, v_texcoord0, v_color0, v_lightmap_uv, v_object_position, v_object_normal

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = vec4_splat(0.0);
}
