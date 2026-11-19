typedef struct { char pad[0x14C]; int cur; int pad2; char buf[1]; } Tool;
extern void func_0014EE48(void *buf, int v);
extern int func_001F9730(void);

void func_001F9910(Tool *t)
{
    func_0014EE48(t->buf, t->cur);
    t->cur = func_001F9730();
}
