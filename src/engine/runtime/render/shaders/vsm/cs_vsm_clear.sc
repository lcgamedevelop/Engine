#include <bgfx_compute.sh>
BUFFER_WO(vsmDemands, uint, 4);
uniform vec4 u_vsm_feedback;
NUM_THREADS(64, 1, 1)
void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index < uint(u_vsm_feedback.x)) vsmDemands[index] = 0u;
}
