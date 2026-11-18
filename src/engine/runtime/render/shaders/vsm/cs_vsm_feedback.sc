#include <bgfx_compute.sh>
BUFFER_RW(vsmDemands, uint, 4);
BUFFER_RO(b_light_data, vec4, 5);
SAMPLER2D(s_vsm_receiver_depth, 0);
uniform vec4 u_vsm_feedback;
uniform mat4 u_vsm_inverse;
#include "vsm_address.sh"
const uint invalid_receiver_page = 0xffffffffu;

uint receiver_page(vec2 pixel)
{
    if (any(greaterThanEqual(pixel, u_vsm_feedback.xy))) return invalid_receiver_page;
    vec2 uv = (pixel + 0.5) / u_vsm_feedback.xy;
    float depth = texture2DLod(s_vsm_receiver_depth, uv, 0.0).r;
    if (depth >= 1.0) return invalid_receiver_page;
    vec2 ndc = uv * 2.0 - 1.0;
    if (u_vsm_feedback.w < 0.5) ndc.y = -ndc.y;
    if (u_vsm_feedback.z > 0.5) depth = depth * 2.0 - 1.0;
    vec4 receiver = mul(u_vsm_inverse, vec4(ndc, depth, 1.0));
    if (abs(receiver.w) < 0.000001) return invalid_receiver_page;
    int level;
    vec2 coordinate;
    uint address;
    return vsm_address(receiver.xyz / receiver.w, level, coordinate, address)
        ? address : invalid_receiver_page;
}

NUM_THREADS(8, 8, 1)
void main()
{
    vec2 pixel = vec2(gl_GlobalInvocationID.xy * 2u);
    uvec4 pages = uvec4(receiver_page(pixel), receiver_page(pixel + vec2(1.0, 0.0)),
                       receiver_page(pixel + vec2(0.0, 1.0)),
                       receiver_page(pixel + vec2(1.0, 1.0)));
    if (pages.x == pages.y && pages.x == pages.z && pages.x == pages.w)
    {
        if (pages.x != invalid_receiver_page)
        {
            atomicOr(vsmDemands[pages.x / 32u], 1u << (pages.x % 32u));
        }
        return;
    }

    for (int lane = 0; lane < 4; ++lane)
    {
        uint page = pages[lane];
        if (page == invalid_receiver_page) continue;
        uint word = page / 32u;
        uint mask = 1u << (page % 32u);
        for (int neighbor = lane + 1; neighbor < 4; ++neighbor)
        {
            uint neighbor_page = pages[neighbor];
            if (neighbor_page != invalid_receiver_page && neighbor_page / 32u == word)
            {
                mask |= 1u << (neighbor_page % 32u);
                pages[neighbor] = invalid_receiver_page;
            }
        }
        atomicOr(vsmDemands[word], mask);
    }
}
