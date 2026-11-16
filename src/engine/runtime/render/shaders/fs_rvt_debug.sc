$input v_texcoord0
#include <bgfx_shader.sh>
#include "color_space.sh"
#include "rvt_cache.sh"
// mode(0 virtual,1 atlas,2 page table), channel, mip, available.
uniform vec4 u_rvt_debug;
void main()
{
    vec2 uv=v_texcoord0;vec3 color=vec3(0.06,0.07,0.09);
    if(u_rvt_debug.w<0.5) {gl_FragColor=vec4(color,0.95);return;}
    vec2 address=uv;float mip=0.0;
    float selected_mip = u_rvt_debug.z < 0.0 ? u_rvt_parameters.z : u_rvt_debug.z;
    bool found=u_rvt_debug.x>0.5 && u_rvt_debug.x<1.5;
    if(!found) found=rvt_address(uv,selected_mip,address,mip);
    if(u_rvt_debug.x>1.5)
    {
        float count=u_rvt_layout.w/exp2(selected_mip);
        vec4 page=texture2DLod(s_rvt_table,(floor(uv*count)+0.5)/count,selected_mip);
        color=page.a>0.5 ? vec3(fract(page.x*0.618+0.25),fract(page.y*0.414+0.3),0.55) : vec3(0.32,0.12,0.08);
        vec2 edge=min(fract(uv*count),1.0-fract(uv*count));
        if(min(edge.x,edge.y)<0.025) color=vec3_splat(0.7);
    }
    else if(found)
    {
        vec4 base=texture2DArrayLod(s_rvt_atlas,vec3(address,0.0),0.0);
        vec4 normal=texture2DArrayLod(s_rvt_atlas,vec3(address,1.0),0.0);
        vec3 orm=texture2DArrayLod(s_rvt_atlas,vec3(address,2.0),0.0).rgb;
        if(base.a>0.5)
        {
            if(u_rvt_debug.y<0.5) color=linear_to_display_srgb(base.rgb);
            else if(u_rvt_debug.y<1.5) color=normalize(normal.xyz*2.0-1.0)*0.5+0.5;
            else if(u_rvt_debug.y<2.5) color=vec3_splat(orm.g);
            else if(u_rvt_debug.y<3.5) color=vec3_splat(orm.b);
            else color=vec3_splat(orm.r);
        }
    }
    else color=mix(vec3(0.13,0.14,0.16),vec3(0.24,0.13,0.08),mod(floor(uv.x*32.0)+floor(uv.y*32.0),2.0));
    gl_FragColor=vec4(color,1.0);
}
