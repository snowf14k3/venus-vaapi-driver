#!/bin/sh
set -eu

NM=$1
DRIVER=$2

"$NM" -D --defined-only "$DRIVER" |
    grep -Eq '[[:space:]]__vaDriverInit_1_[0-9]+$'

echo "PASS: libva driver init symbol exported"
