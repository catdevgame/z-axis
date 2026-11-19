typedef struct { int used; char pad[0x18E40 - 4]; } Slot; /* 0x18E40 bytes */
typedef struct {
    int id;      /* 0x00 */
    Slot *slots; /* 0x04 */
    int f8;      /* 0x08 */
    int fC;      /* 0x0C */
    int count;   /* 0x10 */
} SlotPool;

void func_00116C98(SlotPool *pool, int id, Slot *slots, int count)
{
    int i;

    pool->fC = 0;
    pool->id = id;
    pool->slots = slots;
    pool->count = count;
    pool->f8 = 0;
    for (i = 0; i < count; i++) {
        pool->slots[i].used = 0;
    }
}
