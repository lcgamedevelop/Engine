// Terrain layer evaluation feeds the same PBR lighting and RVT writer as ordinary materials.
#ifndef ENGINE_TERRAIN_SURFACE_SH
#define ENGINE_TERRAIN_SURFACE_SH
#include <bgfx_shader.sh>
#include "pbr_surface_types.sh"
SAMPLER2DARRAY(s_layer_base, 0);
SAMPLER2DARRAY(s_layer_normal, 1);
SAMPLER2DARRAY(s_layer_orm, 2);
SAMPLER2D(s_layer_palette, 3);
uniform mat4 u_normal_transform;
uniform vec4 u_surface_origin;
uniform vec4 u_material;
uniform vec4 u_specular;
uniform vec4 u_specular_options;
uniform vec4 u_surface_parameters;
uniform vec4 u_surface;

bool pbr_orthographic_view()
{
    return abs(mul(u_proj,vec4(0.0,0.0,0.0,1.0)).w)>0.5;
}

vec3 pbr_view_direction(vec3 camera_position, vec3 world_position)
{
    vec3 direction = pbr_orthographic_view()
        ? mul(u_invView,vec4(0.0,0.0,1.0,0.0)).xyz : camera_position-world_position;
    return direction/max(length(direction),0.0001);
}
vec4 terrain_palette(float layer, float column)
{
    return texture2D(s_layer_palette,vec2((column+0.5)/4.0,(layer+0.5)/256.0));
}
void terrain_layer(float layer, float weight, vec3 p, vec3 n, vec3 axis_weights,
                   inout vec3 color, inout vec3 orm, inout vec3 perturbation, inout vec3 emission)
{
    if (weight <= 0.00001) return;
    vec4 tint=terrain_palette(layer,0.0);
    vec4 properties=terrain_palette(layer,1.0);
    vec4 uv=terrain_palette(layer,2.0);
    vec3 signs=vec3(n.x<0.0?-1.0:1.0,n.y<0.0?-1.0:1.0,n.z<0.0?-1.0:1.0);
    vec2 xuv=vec2(-p.z*signs.x,p.y)*uv.xy+uv.zw;
    vec2 yuv=vec2(p.x,-p.z*signs.y)*uv.xy+uv.zw;
    vec2 zuv=vec2(p.x*signs.z,p.y)*uv.xy+uv.zw;
    vec3 base=texture2DArray(s_layer_base,vec3(xuv,layer)).rgb*axis_weights.x
             +texture2DArray(s_layer_base,vec3(yuv,layer)).rgb*axis_weights.y
             +texture2DArray(s_layer_base,vec3(zuv,layer)).rgb*axis_weights.z;
    vec3 packed=texture2DArray(s_layer_orm,vec3(xuv,layer)).rgb*axis_weights.x
             +texture2DArray(s_layer_orm,vec3(yuv,layer)).rgb*axis_weights.y
             +texture2DArray(s_layer_orm,vec3(zuv,layer)).rgb*axis_weights.z;
    vec3 nx=texture2DArray(s_layer_normal,vec3(xuv,layer)).xyz*2.0-1.0;
    vec3 ny=texture2DArray(s_layer_normal,vec3(yuv,layer)).xyz*2.0-1.0;
    vec3 nz=texture2DArray(s_layer_normal,vec3(zuv,layer)).xyz*2.0-1.0;
    // Blend tangent perturbations against the geometric normal, so flat maps preserve it.
    vec3 bump=vec3(0.0,nx.y,-nx.x*signs.x)*axis_weights.x
             +vec3(ny.x,0.0,-ny.y*signs.y)*axis_weights.y
             +vec3(nz.x*signs.z,nz.y,0.0)*axis_weights.z;
    color+=base*tint.rgb*weight;
    orm+=packed*vec3(1.0,properties.y,properties.x)*weight;
    perturbation+=bump*properties.z*weight;
    emission+=terrain_palette(layer,3.0).rgb*weight;
}
PbrSurface sample_terrain_surface(vec2 encoded, vec4 weights, vec3 object_position, vec3 object_normal)
{
    vec4 ids=vec4(mod(floor(encoded.x+0.5),256.0),floor((encoded.x+0.5)/256.0),
                 mod(floor(encoded.y+0.5),256.0),floor((encoded.y+0.5)/256.0));
    weights=max(weights,vec4_splat(0.0));weights/=max(dot(weights,vec4_splat(1.0)),0.00001);
    vec3 n=normalize(object_normal);
    vec3 axes=pow(abs(n),vec3_splat(4.0));axes/=max(dot(axes,vec3_splat(1.0)),0.00001);
    vec3 p=object_position+u_surface_origin.xyz;
    vec3 color=vec3_splat(0.0),orm=vec3_splat(0.0),bump=vec3_splat(0.0),emission=vec3_splat(0.0);
    terrain_layer(ids.x,weights.x,p,n,axes,color,orm,bump,emission);
    terrain_layer(ids.y,weights.y,p,n,axes,color,orm,bump,emission);
    terrain_layer(ids.z,weights.z,p,n,axes,color,orm,bump,emission);
    terrain_layer(ids.w,weights.w,p,n,axes,color,orm,bump,emission);
    PbrSurface result;
    result.base_color=vec4(color,1.0);
    result.normal=normalize(mul(u_normal_transform,vec4(normalize(n+bump),0.0)).xyz);
    result.metallic=saturate(orm.z);result.roughness=clamp(orm.y,0.045,1.0);
    result.specular_color=mix(vec3_splat(0.04),color,result.metallic);
    result.diffuse_weight=1.0-result.metallic;
    result.smoothness=1.0-result.roughness;result.occlusion=saturate(orm.x);
    result.emission=emission;
    return result;
}
#define sample_pbr_surface(uv, weights, normal, tangent, view) sample_terrain_surface(uv, weights, v_object_position, v_object_normal)
#endif
