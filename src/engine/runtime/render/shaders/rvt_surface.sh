#include "rvt_cache.sh"
void apply_rvt(inout PbrSurface surface,vec3 position,vec3 geometric_normal)
{
    if(u_rvt_parameters.x<0.5 || geometric_normal.y<0.25 || u_specular.w>0.5 || u_material.z>0.5) return;
    vec2 uv=(position.xz-u_rvt_bounds.xy)*u_rvt_bounds.zw;
    vec2 footprint=max(abs(dFdx(uv)),abs(dFdy(uv)))*u_rvt_layout.w*u_rvt_layout.x;
    float wanted=clamp(log2(max(max(footprint.x,footprint.y),1.0)),0.0,u_rvt_parameters.y);
    vec2 address;float mip;
    bool resident=rvt_address(uv,wanted,address,mip);
    if(resident)
    {
        vec4 color=texture2DArrayLod(s_rvt_atlas,vec3(address,0.0),0.0);
        vec4 normal_height=texture2DArrayLod(s_rvt_atlas,vec3(address,1.0),0.0);
        vec3 orm=texture2DArrayLod(s_rvt_atlas,vec3(address,2.0),0.0).rgb;
        float height=normal_height.w/max(u_rvt_height.y,0.000001)+u_rvt_height.x;
        float texel=max(1.0/u_rvt_bounds.z,1.0/u_rvt_bounds.w)*exp2(mip)/(u_rvt_layout.w*u_rvt_layout.x);
        // Top-surface cache never projects through cave roofs or onto overhang undersides.
        if(color.a>0.99 && abs(position.y-height)<=max(0.05,texel*1.5))
        {
            surface.base_color.rgb=color.rgb;
            surface.normal=normalize(normal_height.xyz*2.0-1.0);
            surface.occlusion=orm.r;surface.roughness=max(orm.g,0.045);surface.metallic=orm.b;
            surface.diffuse_weight=1.0-surface.metallic;
            surface.specular_color=mix(vec3_splat(0.04),color.rgb,surface.metallic);
            surface.smoothness=1.0-surface.roughness;
        }
    }
    float diagnostic=u_rvt_parameters.w;
    if(mod(floor(diagnostic/2.0),2.0)>0.5) surface.base_color.rgb=resident ? vec3(0.1,0.7,0.3) : vec3(0.8,0.2,0.1);
    if(mod(floor(diagnostic/4.0),2.0)>0.5 && !resident) surface.base_color.rgb=vec3(0.95,0.4,0.05);
    if(mod(diagnostic,2.0)>0.5)
    {
        float count=u_rvt_layout.w/exp2(resident?mip:wanted);
        vec2 cell=fract(uv*count),edge=min(cell,1.0-cell);
        if(min(edge.x,edge.y)<0.015) surface.base_color.rgb=vec3(1.0,0.8,0.1);
    }
}
