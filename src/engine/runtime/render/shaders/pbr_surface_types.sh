#ifndef ENGINE_PBR_SURFACE_TYPES
#define ENGINE_PBR_SURFACE_TYPES

#if defined(ENGINE_PBR_CAMERA_PASS)
#define ENGINE_PBR_LIGHTING_MODE 0.0
#else
#define ENGINE_PBR_LIGHTING_MODE (u_material.z)
#endif

struct PbrSurface
{
    vec4 base_color;
    vec3 normal;
    vec3 specular_color;
    vec3 emission;
    float metallic;
    float roughness;
    float diffuse_weight;
    float smoothness;
    float occlusion;
};

#endif
