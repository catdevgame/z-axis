extern struct { int active; int pad[7]; } D_00299250; /* > 16 bytes: not gp-relative */
extern int func_0011A2B8(void);

int func_00147E40(void)
{
    if (D_00299250.active == 0) {
        return 0;
    }
    return func_0011A2B8();
}
