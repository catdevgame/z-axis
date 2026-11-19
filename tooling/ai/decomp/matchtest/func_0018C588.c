extern int D_002458B8[][9];

/* Index of value in row `row` of a 9-int table (assumes it is present). */
int func_0018C588(int value, int row)
{
    int *p = D_002458B8[row];
    int i = 0;

    while (1) {
        if (*p++ == value) {
            return i;
        }
        i++;
    }
}
