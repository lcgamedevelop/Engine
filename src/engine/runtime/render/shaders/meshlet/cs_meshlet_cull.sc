// Indexed indirect submission follows bgfx example 48 (BSD-2-Clause).
// Copyright 2022 Liam Twigger. See third_party/notices/bgfx.txt.
#include "bgfx_compute.sh"

BUFFER_RO(meshletDescriptors, vec4, 0);
BUFFER_RO(meshletInputs, vec4, 1);
BUFFER_WO(meshletInstances, vec4, 2);
BUFFER_WO(meshletCommands, uvec4, 3);
BUFFER_RW(meshletCounters, uint, 4);
// First command, frustum enabled, orthographic, sample counters.
uniform vec4 u_meshlet_parameters;
uniform vec4 u_meshlet_planes[6];
SHARED uint visibleCount;
SHARED uint clusterVisible[32];

bool meshlet_visible(uint record, uint source)
{
    vec4 sphere = meshletDescriptors[record];
    vec4 cone = meshletDescriptors[record + 1u];
    if (u_meshlet_parameters.y > 0.5)
    {
        vec4 column0 = meshletInputs[source];
        vec4 column1 = meshletInputs[source + 1u];
        vec4 column2 = meshletInputs[source + 2u];
        vec4 column3 = meshletInputs[source + 3u];
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
                return false;
            }
        }
    }
    vec4 camera = meshletInputs[source + 7u];
    if (camera.w > 0.5 && cone.w < 1.0)
    {
        bool orthographic = u_meshlet_parameters.z > 0.5;
        vec3 direction = orthographic ? camera.xyz : sphere.xyz - camera.xyz;
        float radius = orthographic ? 0.0 : sphere.w;
        float threshold = cone.w * length(direction) + radius;
        return dot(direction, cone.xyz) < threshold + 0.00001 * (threshold + 1.0);
    }
    return true;
}

void emit_draw(uint command, uint firstIndex, uint indexCount, uint instances,
               uint firstInstance, uint retainedPairs)
{
    drawIndexedIndirect(meshletCommands, command, indexCount, instances, firstIndex, 0u,
                        firstInstance);
    if (u_meshlet_parameters.w > 0.5 && instances > 0u)
    {
        atomicAdd(meshletCounters[0], retainedPairs);
        atomicAdd(meshletCounters[1], 1u);
        atomicAdd(meshletCounters[2], (indexCount / 3u) * instances);
    }
}

NUM_THREADS(32, 1, 1)
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uint command = floatBitsToUint(u_meshlet_parameters.x) + gl_WorkGroupID.x;
    // First cluster record, cluster count, first input instance, instance count.
    uvec4 work = floatBitsToUint(meshletDescriptors[command]);
    if (work.w == 1u)
    {
        if (lane < work.y)
        {
            clusterVisible[lane] = meshlet_visible(work.x + lane * 3u, work.z * 8u) ? 1u : 0u;
        }
        barrier();
        if (lane == 0u)
        {
            uint first = work.y;
            uint last = 0u;
            for (uint cluster = 0u; cluster < work.y; ++cluster)
            {
                if (clusterVisible[cluster] > 0u)
                {
                    first = min(first, cluster);
                    last = cluster;
                }
            }
            if (first == work.y)
            {
                emit_draw(command, 0u, 0u, 0u, 0u, 0u);
            }
            else
            {
                // Keep the original contiguous index order and conservatively include gaps.
                uvec4 begin = floatBitsToUint(meshletDescriptors[work.x + first * 3u + 2u]);
                uvec4 end = floatBitsToUint(meshletDescriptors[work.x + last * 3u + 2u]);
                emit_draw(command, begin.x, end.x + end.y - begin.x, 1u, begin.z,
                          last - first + 1u);
            }
        }
        return;
    }

    uvec4 indices = floatBitsToUint(meshletDescriptors[work.x + 2u]);
    if (lane == 0u)
    {
        visibleCount = 0u;
    }
    barrier();
    for (uint instance = lane; instance < work.w; instance += 32u)
    {
        uint source = (work.z + instance) * 8u;
        if (meshlet_visible(work.x, source))
        {
            uint slot;
            atomicFetchAndAdd(visibleCount, 1u, slot);
            uint destination = (indices.z + slot) * 7u;
            for (uint column = 0u; column < 7u; ++column)
            {
                meshletInstances[destination + column] = meshletInputs[source + column];
            }
        }
    }
    barrier();
    if (lane == 0u)
    {
        emit_draw(command, indices.x, indices.y, visibleCount, indices.z, visibleCount);
    }
}
