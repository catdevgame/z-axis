typedef struct { short f0; unsigned short refs; } Res;
typedef struct { char pad[0x44]; Res *a; Res *b; Res *c; } Holder;
/* An enum selector matters: gcc 2.95 only uses its char-cost table to balance the
   case tree for non-enum selectors; enums also compare unsigned (sltiu). */
typedef enum { SLOT_A = 10, SLOT_B = 11, SLOT_C = 12 } SlotKind;

void func_00134018(Holder *h, SlotKind slot, Res *r)
{
    switch (slot) {
    case SLOT_A:
        h->a = r;
        break;
    case SLOT_B:
        h->b = r;
        break;
    case SLOT_C:
        h->c = r;
        break;
    default:
        return;
    }
    r->refs++;
}
