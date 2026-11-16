#include "bgfx_compute.sh"

BUFFER_WO(meshletCounters, uint, 4);

NUM_THREADS(1, 1, 1)
void main()
{
    meshletCounters[0] = 0u;
    meshletCounters[1] = 0u;
    meshletCounters[2] = 0u;
    meshletCounters[3] = 0u;
}
