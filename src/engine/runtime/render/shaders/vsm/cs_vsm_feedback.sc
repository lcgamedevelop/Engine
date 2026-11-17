#include <bgfx_compute.sh>
BUFFER_RW(vsmDemands, uint, 4);
BUFFER_RO(b_light_data, vec4, 5);
SAMPLER2D(s_vsm_receiver_depth, 0);
uniform vec4 u_vsm_feedback;
uniform mat4 u_vsm_inverse;
#include "vsm_address.sh"
NUM_THREADS(8, 8, 1)
void main()
{
    vec2 pixel = vec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel, u_vsm_feedback.xy))) return;
    vec2 uv = (pixel + 0.5) / u_vsm_feedback.xy;
    float depth = texture2DLod(s_vsm_receiver_depth, uv, 0.0).r;
    if (depth >= 1.0) return;
    vec2 ndc = uv * 2.0 - 1.0;
    if (u_vsm_feedback.w < 0.5) ndc.y = -ndc.y;
    if (u_vsm_feedback.z > 0.5) depth = depth * 2.0 - 1.0;
    vec4 receiver = mul(u_vsm_inverse, vec4(ndc, depth, 1.0));
    if (abs(receiver.w) < 0.000001) return;
    int level;
    vec2 coordinate;
    uint address;
    if (vsm_address(receiver.xyz / receiver.w, level, coordinate, address))
        atomicOr(vsmDemands[address / 32u], 1u << (address % 32u));
}
