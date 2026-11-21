#!/bin/sh
# usage: showfunc.sh func_XXXXXXXX  -> prints the function's asm from the split
awk -v f="$1" '$0 ~ "^nonmatching "f"," {p=1} p {print} p && $0 ~ "^endlabel "f"$" {exit}' asm/cod/000000.s
