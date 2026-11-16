#ifndef ENGINE_PBR_SURFACE_TYPES
#define ENGINE_PBR_SURFACE_TYPES
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
