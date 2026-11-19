extern void *func_00164428(void);
extern void *func_001359E8(void *p, int a, int b);

void *func_001E5488(void)
{
    void *p = func_00164428();
    if (p != 0) {
        return func_001359E8(p, 8, 0x1C);
    }
    return 0;
}
