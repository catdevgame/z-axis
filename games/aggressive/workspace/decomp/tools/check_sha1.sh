#!/bin/sh
# usage: check_sha1.sh <expected-sha1> <file>
got=$(shasum -a 1 "$2" | cut -d' ' -f1)
if [ "$got" = "$1" ]; then echo "$2: OK ($got)"; exit 0; fi
echo "$2: MISMATCH (got $got, want $1)"; exit 1
