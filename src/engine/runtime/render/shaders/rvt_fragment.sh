#include <bgfx_shader.sh>
#include "pbr_helpers.sh"
#if defined(ENGINE_TERRAIN_SURFACE)
#include "terrain_surface.sh"
#else
#include "pbr_surface.sh"
#endif
uniform vec4 u_rvt_height;
void main()
{
    // Evaluate only the source material. Never sample the destination virtual texture here.
    PbrSurface s=sample_pbr_surface(v_texcoord0,v_color0,v_world_normal,v_world_tangent,vec3(0.0,1.0,0.0));
    if(v_world_normal.y<=0.05 || s.base_color.a<0.01) discard;
    gl_FragData[0]=vec4(s.base_color.rgb,1.0);
    gl_FragData[1]=vec4(s.normal*0.5+0.5,saturate((v_world_position.y-u_rvt_height.x)*u_rvt_height.y));
    gl_FragData[2]=vec4(s.occlusion,s.roughness,s.metallic,1.0);
}
