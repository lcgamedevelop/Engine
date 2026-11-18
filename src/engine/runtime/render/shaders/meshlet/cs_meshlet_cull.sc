// Indexed indirect submission follows bgfx example 48 (BSD-2-Clause).
// Copyright 2022 Liam Twigger. See third_party/notices/bgfx.txt.
#include "bgfx_compute.sh"

BUFFER_RO(meshletDescriptors, vec4, 0);
BUFFER_RO(meshletInputs, vec4, 1);
BUFFER_WO(meshletInstances, vec4, 2);
BUFFER_WO(meshletCommands, uvec4, 3);
BUFFER_RW(meshletCounters, uint, 4);
uniform vec4 u_meshlet_parameters[3];
uniform vec4 u_meshlet_planes[6];
SHARED uint visibleCount;

NUM_THREADS(64, 1, 1)
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uint cluster = floatBitsToUint(u_meshlet_parameters[0].x) + gl_WorkGroupID.x;
    vec4 sphere = meshletDescriptors[cluster * 4u];
    vec4 cone = meshletDescriptors[cluster * 4u + 1u];
    uvec4 topology = floatBitsToUint(meshletDescriptors[cluster * 4u + 2u]);
    uvec4 batch_info = floatBitsToUint(meshletDescriptors[cluster * 4u + 3u]);
    uint firstInstance = batch_info.z;
    if (lane == 0u)
    {
        visibleCount = 0u;
    }
    barrier();
    for (uint instance = lane; instance < batch_info.y; instance += 64u)
    {
        uint source = (batch_info.x + instance) * 8u;
        vec4 column0 = meshletInputs[source];
        vec4 column1 = meshletInputs[source + 1u];
        vec4 column2 = meshletInputs[source + 2u];
        vec4 column3 = meshletInputs[source + 3u];
        bool visible = true;
        if (u_meshlet_parameters[2].x > 0.5)
        {
            for (int p = 0; p < 6; ++p)
            {
                vec4 plane = u_meshlet_planes[p];
                vec3 localNormal = vec3(dot(plane, column0), dot(plane, column1),
                                        dot(plane, column2));
                float distance = dot(localNormal, sphere.xyz) + dot(plane, column3);
                float radius = sphere.w * length(localNormal);
                float tolerance = 0.00001 * (abs(distance) + radius + 1.0);
                if (distance + radius < -tolerance)
                {
                    visible = false;
                }
            }
        }
        vec4 camera = meshletInputs[source + 7u];
        if (visible && camera.w > 0.5 && cone.w < 1.0)
        {
            vec3 direction = u_meshlet_parameters[2].y > 0.5 ? camera.xyz
                                                               : sphere.xyz - camera.xyz;
            float radius = u_meshlet_parameters[2].y > 0.5 ? 0.0 : sphere.w;
            float threshold = cone.w * length(direction) + radius;
            visible = dot(direction, cone.xyz) < threshold + 0.00001 * (threshold + 1.0);
        }
        if (visible)
        {
            uint slot;
            atomicFetchAndAdd(visibleCount, 1u, slot);
            if (batch_info.y > 1u)
            {
                uint destination = (firstInstance + slot) * 7u;
                for (uint column = 0u; column < 7u; ++column)
                {
                    meshletInstances[destination + column] = meshletInputs[source + column];
                }
            }
        }
    }
    barrier();
    if (lane == 0u)
    {
        drawIndexedIndirect(meshletCommands, cluster, topology.y, visibleCount,
                            topology.x, 0u, firstInstance);
        if (u_meshlet_parameters[2].z > 0.5)
        {
            atomicAdd(meshletCounters[0], visibleCount);
            atomicAdd(meshletCounters[1], visibleCount > 0u ? 1u : 0u);
            atomicAdd(meshletCounters[2], (topology.y / 3u) * visibleCount);
        }
    }
}
