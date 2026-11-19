#include "bgfx_compute.sh"

BUFFER_RO(meshletIndexJobs, uvec4, 0);
BUFFER_RW(meshletIndexCommands, uvec4, 1);
BUFFER_RAW_RW(meshletIndexArena, 2);
BUFFER_RW(meshletIndexCounters, uint, 3);
uniform vec4 u_meshlet_index_compaction;
SHARED uvec2 sourceRanges[64];
SHARED uint vertexOffsets[64];
SHARED uint destinationOffsets[64];
SHARED uint copyRequired;

uint readIndex(uint index)
{
    uint word = rawLoadUint(meshletIndexArena, index / 2u);
    return (word >> ((index & 1u) * 16u)) & 0xffffu;
}

uint firstIndexInNextRange(uint first, uint end)
{
    for (uint i = first; i < end; ++i)
    {
        uvec2 range = sourceRanges[i];
        if (range.y != 0u)
        {
            return readIndex(range.x) + vertexOffsets[i];
        }
    }
    return 0u;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uint job = gl_WorkGroupID.x;
    // Source delta or span table start, scratch start, first command, packed counts.
    uvec4 work = meshletIndexJobs[job];
    uint commandCount = work.w & 0xffffu;
    uint spanCount = work.w >> 16u;
    if (lane < commandCount)
    {
        uint sourceOffset = work.x;
        uint vertexOffset = 0u;
        for (uint i = 0u; i < spanCount; ++i)
        {
            // Each span stores the exclusive command limit, source delta and vertex base.
            uvec4 span = meshletIndexJobs[work.x + i];
            if (lane < span.x)
            {
                sourceOffset = span.y;
                vertexOffset = span.z;
                break;
            }
        }
        uvec4 args = meshletIndexCommands[(work.z + lane) * 2u];
        sourceRanges[lane] = uvec2(args.z + sourceOffset, args.y != 0u ? args.x : 0u);
        vertexOffsets[lane] = vertexOffset;
    }
    barrier();

    if (lane == 0u)
    {
        uint indexCount = 0u;
        uint firstIndex = 0u;
        uint firstInstance = 0u;
        uint retainedCommands = 0u;
        bool contiguous = true;
        for (uint i = 0u; i < commandCount; ++i)
        {
            uvec2 range = sourceRanges[i];
            if (range.y == 0u)
            {
                continue;
            }
            if (indexCount == 0u)
            {
                firstIndex = range.x;
                firstInstance = meshletIndexCommands[(work.z + i) * 2u + 1u].x;
            }
            else
            {
                contiguous = contiguous && range.x == firstIndex + indexCount;
            }
            contiguous = contiguous && vertexOffsets[i] == 0u;
            destinationOffsets[i] = indexCount;
            indexCount += range.y;
            retainedCommands += 1u;
        }

        copyRequired = contiguous ? 0u : 1u;
        uint destination = floatBitsToUint(u_meshlet_index_compaction.x) + job;
        uint instances = indexCount != 0u ? 1u : 0u;
        drawIndexedIndirect(meshletIndexCommands, destination, indexCount, instances,
                            contiguous ? firstIndex : work.y, 0u, firstInstance);
        if (u_meshlet_index_compaction.y > 0.5 && retainedCommands > 1u)
        {
            atomicAdd(meshletIndexCounters[1], 1u - retainedCommands);
        }
    }
    barrier();
    if (copyRequired == 0u)
    {
        return;
    }

    // Eight lanes copy each retained span. The span with a word's low half owns both halves.
    for (uint i = lane / 8u; i < commandCount; i += 8u)
    {
        uvec2 range = sourceRanges[i];
        if (range.y == 0u)
        {
            continue;
        }
        uint offset = destinationOffsets[i];
        uint firstWord = (offset + 1u) / 2u;
        uint endWord = (offset + range.y + 1u) / 2u;
        uint vertexOffset = vertexOffsets[i];
        for (uint word = firstWord + lane % 8u; word < endWord; word += 8u)
        {
            uint index = word * 2u - offset;
            uint source = range.x + index;
            uint packed = rawLoadUint(meshletIndexArena, source / 2u);
            bool last = index + 1u == range.y;
            if ((source & 1u) != 0u)
            {
                packed >>= 16u;
                if (!last)
                {
                    packed |= rawLoadUint(meshletIndexArena, source / 2u + 1u) << 16u;
                }
            }
            packed = ((packed & 0xffffu) + vertexOffset) |
                     (((packed >> 16u) + vertexOffset) << 16u);
            if (last)
            {
                uint next = firstIndexInNextRange(i + 1u, commandCount);
                packed = (packed & 0xffffu) | (next << 16u);
            }
            rawStoreUint(meshletIndexArena, work.y / 2u + word, packed);
        }
    }
}
