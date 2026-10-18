#!/bin/sh
# Turn a libretro core (archives and/or objects) into an archive holding a
# single relocatable object whose only global symbols are the core's
# retro_* entry points, renamed to <prefix>_retro_*. Every other symbol
# the core defines becomes local, so several cores (and the frontend) can
# be linked into one static binary without clashing on retro_* or on
# generic names like reg, vram or init_sound.
#
#   core-archive.sh PREFIX OUT.a [-k SYMBOL]... INPUT...
#
# -k keeps an extra symbol global (also renamed to PREFIX_SYMBOL), for
# core internals the frontend reads directly. Uses $CROSS_COMPILE tools.
set -eu

prefix=$1
out=$2
shift 2
keep=
while [ "${1:-}" = -k ]; do
	keep="$keep $2"
	shift 2
done
[ $# -gt 0 ] || { echo "core-archive.sh: no inputs" >&2; exit 1; }

tmp=$out.tmp
rm -rf "$tmp"
mkdir -p "$tmp"

# Whole archives: retro_* are only reached through the renamed names, so
# the linker could not pull members on demand anyway.
"${CROSS_COMPILE}ld" -r -o "$tmp/joined.o" --whole-archive "$@" --no-whole-archive

"${CROSS_COMPILE}nm" -g --defined-only "$tmp/joined.o" | awk '{ print $3 }' |
	grep '^retro_' > "$tmp/keep" || true
[ -s "$tmp/keep" ] || { echo "core-archive.sh: no retro_* symbols in $*" >&2; exit 1; }
for s in $keep; do
	"${CROSS_COMPILE}nm" -g --defined-only "$tmp/joined.o" | awk '{ print $3 }' |
		grep -qx "$s" || { echo "core-archive.sh: $s not defined" >&2; exit 1; }
	echo "$s" >> "$tmp/keep"
done
sed "s/.*/& ${prefix}_&/" "$tmp/keep" > "$tmp/rename"

"${CROSS_COMPILE}objcopy" --keep-global-symbols="$tmp/keep" \
	"$tmp/joined.o" "$tmp/local.o"
"${CROSS_COMPILE}objcopy" --redefine-syms="$tmp/rename" \
	"$tmp/local.o" "$tmp/${prefix}_core.o"

rm -f "$out"
"${CROSS_COMPILE}ar" rcD "$out" "$tmp/${prefix}_core.o"
rm -rf "$tmp"
