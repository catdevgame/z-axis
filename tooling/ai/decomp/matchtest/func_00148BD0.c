extern struct { int count; void *a; void *b; int pad[5]; } D_00299570;
extern void func_00138910(void *p);

int func_00148BD0(void)
{
    func_00138910(D_00299570.b);
    func_00138910(D_00299570.a);
    D_00299570.count = 0;
    return 1;
}
