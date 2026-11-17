#ifndef ENGINE_RVT_CACHE_SH
#define ENGINE_RVT_CACHE_SH
SAMPLER2DARRAY(s_rvt_atlas,8);
SAMPLER2D(s_rvt_table,9);
// World XZ minimum and reciprocal extent.
uniform vec4 u_rvt_bounds;
// Page texels, border texels, atlas texels, base page-table size.
uniform vec4 u_rvt_layout;
// enabled, maximum mip, minimum demanded mip, diagnostic bit flags.
uniform vec4 u_rvt_parameters;
uniform vec4 u_rvt_height;

bool rvt_address(vec2 uv,float wanted,out vec2 address,out float resolved)
{
    address=vec2_splat(0.0);resolved=0.0;
    if(any(lessThan(uv,vec2_splat(0.0))) || any(greaterThanEqual(uv,vec2_splat(1.0)))) return false;
    for(int step=0;step<9;++step)
    {
        float mip=min(floor(wanted)+float(step),u_rvt_parameters.y);
        float count=max(1.0,u_rvt_layout.w/exp2(mip));
        vec2 cell=floor(uv*count);
        vec4 page=texture2DLod(s_rvt_table,(cell+0.5)/count,mip);
        if(page.a>0.5)
        {
            vec2 local=fract(uv*count)*u_rvt_layout.x+u_rvt_layout.y;
            address=(page.xy*(u_rvt_layout.x+2.0*u_rvt_layout.y)+local)/u_rvt_layout.z;
            resolved=mip;return true;
        }
        if(mip>=u_rvt_parameters.y) break;
    }
    return false;
}
#endif
