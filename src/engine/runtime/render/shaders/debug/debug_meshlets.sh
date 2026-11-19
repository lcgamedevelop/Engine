// Vertex pulling uses the same bgfx buffer contract as the scene wireframe view.
#include <bgfx_shader.sh>
#include <bgfx_compute.sh>

// Slot 0 is reserved for the instance stream on Metal.
BUFFER_RAW_RO(positions, 1);
BUFFER_RAW_RO(indices, 2);
#include "debug_indices.sh"
BUFFER_RAW_RO(clusterOffsets, 3);
uniform vec4 u_debug_meshlet_parameters[2];

void main()
{
    uvec4 range = floatBitsToUint(u_debug_meshlet_parameters[0]);
    uint index = range.x + gl_VertexID;
    uint first = range.y;
    uint end = first + range.z;
    // Imported descriptors are ordered, nonoverlapping index ranges.
    while (first + 1u < end)
    {
        uint middle = first + (end - first) / 2u;
        uint offset = rawLoadUint(clusterOffsets, middle);
        if (offset <= index)
        {
            first = middle;
        }
        else
        {
            end = middle;
        }
    }
    uint vertex = geometryIndex(index, u_debug_meshlet_parameters[1].z > 0.5);
    uint position = (vertex + uint(u_debug_meshlet_parameters[1].w)) *
                    uint(u_debug_meshlet_parameters[1].x);
    vec3 local = vec3(rawLoadFloat(positions, position), rawLoadFloat(positions, position + 1u),
                      rawLoadFloat(positions, position + 2u));
#if defined(MESHLET_DEBUG_INSTANCED)
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    gl_Position = mul(u_viewProj, mul(model, vec4(local, 1.0)));
#else
    gl_Position = mul(u_modelViewProj, vec4(local, 1.0));
#endif
    uint hash = (range.w ^ (first + uint(u_debug_meshlet_parameters[1].y) * 65537u)) * 16777619u;
    hash = (hash ^ (hash >> 16u)) * 0x7feb352du;
    hash = (hash ^ (hash >> 15u)) * 0x846ca68bu;
    hash ^= hash >> 16u;
    vec3 rgb = vec3(float(hash & 255u), float((hash >> 8u) & 255u),
                    float((hash >> 16u) & 255u));
    v_color = vec4(0.02 + rgb * (0.75 / 255.0), 1.0);
}
