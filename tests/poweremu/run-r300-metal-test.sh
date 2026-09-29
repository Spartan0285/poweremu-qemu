#!/bin/sh
# Standalone host-GPU test; does not boot or modify a guest.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/poweremu-r300-test.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
clang -std=c11 -Wall -Wextra -Werror -DR300_FP_STANDALONE \
    -I"$root/hw/display" -c "$root/hw/display/ppc_mac_gpu_r300_fp.c" -o "$out/fp.o"
clang -std=c11 -Wall -Wextra -Werror -DR300_FP_STANDALONE \
    -I"$root/hw/display" -c "$root/hw/display/ppc_mac_gpu_r300_vp.c" -o "$out/vp.o"
clang -std=c11 -Wall -Wextra -Werror -DR300_FP_STANDALONE \
    -I"$root/hw/display" -c "$root/hw/display/ppc_mac_gpu_r300_data.c" -o "$out/data.o"
clang -std=c11 -Wall -Wextra -Werror -I"$root/hw/display" \
    "$root/tests/poweremu/r300-data-test.c" "$out/data.o" -o "$out/data-test"
"$out/data-test"
clang -fobjc-arc -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -I"$root/hw/display" "$root/tests/poweremu/r300-metal-test.m" "$out/fp.o" "$out/vp.o" "$out/data.o" \
    -framework Foundation -framework Metal -o "$out/test"
"$out/test"
