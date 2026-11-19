#include <bgfx_compute.sh>

SAMPLER2D(s_virtual_shadow, 0);
#if BGFX_SHADER_LANGUAGE_METAL
// bgfx supplies the float2 image helpers but not this format's type token.
#define COMP_rg32f vec2
IMAGE2D_WO(vsmDepthRanges, rg32f, 1);
#else
IMAGE2D_WO(vsmDepthRanges, rgba32f, 1);
#endif
uniform vec4 u_vsm_range_tile_origins[16];
uniform vec4 u_vsm_range_output;

SHARED vec2 lane_ranges[16];

NUM_THREADS(4, 4, 1)
void main()
{
    // One group per 8x8 block. The two-texel halo covers every centred 5x5 filter.
    ivec4 tile = ivec4(u_vsm_range_tile_origins[gl_WorkGroupID.z]);
    ivec2 block = ivec2(gl_WorkGroupID.xy) * 8;
    bool local_face = u_vsm_range_output.y > 0.0;
    if (local_face && any(greaterThan(tile.xy + block, tile.zw)))
    {
        return;
    }
    ivec2 lane = ivec2(gl_LocalInvocationID.xy);
    ivec2 first = tile.xy + block - 2 + lane;
    vec2 range = vec2(1.0, 0.0);
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            ivec2 texel = clamp(first + ivec2(x, y) * 4, tile.xy, tile.zw);
            float depth = texelFetch(s_virtual_shadow, texel, 0).r;
            range = vec2(min(range.x, depth), max(range.y, depth));
        }
    }
    int lane_index = lane.y * 4 + lane.x;
    lane_ranges[lane_index] = range;
    barrier();
    if (lane_index == 0)
    {
        for (int i = 1; i < 16; ++i)
        {
            range = vec2(min(range.x, lane_ranges[i].x), max(range.y, lane_ranges[i].y));
        }
        ivec2 destination = (tile.xy + block) / 8;
        if (local_face)
        {
            int face = int(u_vsm_range_output.z) + int(gl_WorkGroupID.z);
            destination = ivec2(face % 6, face / 6) * int(u_vsm_range_output.y) +
                          ivec2(0, int(u_vsm_range_output.x)) + ivec2(gl_WorkGroupID.xy);
        }
        imageStore(vsmDepthRanges, destination, vec4(range, 0.0, 0.0));
    }
}
