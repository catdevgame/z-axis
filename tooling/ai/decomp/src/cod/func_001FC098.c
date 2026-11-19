typedef struct {
    char pad[0x68];
    int minX, minY, minZ; /* 0x68 */
    int maxX, maxY, maxZ; /* 0x74 */
} Bounds;

int func_001FC098(Bounds *b, int *pos)
{
    if (pos[0] > b->maxX || pos[0] < b->minX || pos[2] > b->maxZ) {
        return 0;
    }
    return pos[2] >= b->minZ;
}
