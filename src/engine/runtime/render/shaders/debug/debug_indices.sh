// Included after the raw index buffer declaration. The last half-word is padded on upload.
uint geometryIndex(uint index, bool shortIndices)
{
    uint word = rawLoadUint(indices, shortIndices ? (index >> 1u) : index);
    return shortIndices ? (word >> ((index & 1u) * 16u)) & 0xffffu : word;
}
