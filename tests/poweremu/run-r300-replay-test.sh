#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/poweremu-r300-replay.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for unit in fp vp data; do
    clang -std=c11 -Wall -Wextra -Werror -DR300_FP_STANDALONE \
      -I"$root/hw/display" -c "$root/hw/display/ppc_mac_gpu_r300_$unit.c" -o "$out/$unit.o"
done
clang -Wall -Wextra -Werror -Wno-deprecated-declarations -I"$root/hw/display" \
    -c "$root/hw/display/ppc_mac_gpu_r300_metal.m" -o "$out/metal.o"
clang -fobjc-arc -Wall -Wextra -Werror -I"$root/hw/display" \
    "$root/tests/poweremu/r300-replay-test.m" "$out/fp.o" "$out/vp.o" "$out/data.o" "$out/metal.o" \
    -framework Foundation -framework Metal -o "$out/test"
"$out/test" "$1"
