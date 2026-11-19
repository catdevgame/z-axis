typedef struct { int pad; void *f4; } Inner;
typedef struct { int id; Inner *inner; } Arg;
extern void *func_001B1A90(int id);
extern void func_00136B60(void *dst, void *src, int size);

int func_001EDF50(Arg *arg)
{
    Inner *in = arg->inner;
    void *p = func_001B1A90(arg->id);
    func_00136B60(in->f4, p, 0x28);
    return 1;
}
