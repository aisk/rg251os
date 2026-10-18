#!/bin/sh
# Host test of the first boot partition grow (app/frontend/sdgrow.c) on
# sparse card images laid out like board/rg351p/genimage.cfg: the
# frontend runs it under qemu-user with this script as its file system
# hook (mtools instead of mounts), then the result is checked with
# fsck.fat, file, minfo and mcopy: the files, their content and mtimes.
# Covers interruptions at every step, a failing check (rollback), the
# attempt limit, a garbage and a blank p3, 32 KiB clusters and the 2 TiB
# limit.
#
#   sdgrow-test.sh QEMU FRONTEND DIR
#   sdgrow-test.sh hook extract|inject IMG OFFSET DIR   (the frontend's hook)

set -e
export PATH=/usr/sbin:/sbin:$PATH TZ=UTC MTOOLS_SKIP_CHECK=1

if [ "$1" = hook ]; then
	img=$3@@$4
	case $2 in
	extract)
		mdir -i "$img" ::/ >/dev/null 2>&1 || exit 1
		if [ -n "$(mdir -i "$img" -b ::/)" ]; then
			mcopy -i "$img" -s -p -m -n "::/*" "$5/"
		fi
		;;
	inject)
		if [ -n "$(ls -A "$5")" ]; then
			mcopy -i "$img" -s -p -m -o "$5"/* ::/
		fi
		;;
	esac
	exit 0
fi

QEMU=$1
FE=$2
DIR=$3
SELF=$(realpath "$0")
P3=131072           # p3 start and size in the image (sectors)
rm -rf "$DIR"
mkdir -p "$DIR"
cd "$DIR"

fail() {
	echo "sdgrow-test: FAILED: $*" >&2
	exit 1
}

# The files of a GBA partition after some use, with even mtimes (FAT).
mkdir -p src/roms src/saves src/config/roms src/log/boot-0001 src/empty
head -c 6000000 /dev/urandom > "src/roms/Some Game (USA, Europe).gba"
head -c 1048576 /dev/urandom > src/roms/b.gba
head -c 131072 /dev/urandom > src/saves/b.sav
i=0
while [ $i -lt 150 ]; do
	echo "state $i" > src/saves/s$i.txt
	i=$((i + 1))
done
printf 'volume=128\n' > src/config/settings.txt
printf 'core=mgba\n' > src/config/roms/b.gba.txt
printf '1\n' > src/log/boot-counter.txt
: > src/log/boot-0001/frontend.txt
find src -exec touch -d '2025-03-04 05:06:08' {} +
touch -d '2024-01-02 03:04:10' src/roms/b.gba

mkfs.vfat -n GBA -C p3.vfat $((P3 / 2)) >/dev/null
mcopy -i p3.vfat -s -p -m src/* ::/

# make_card NAME SIZE: a card with the image's layout, p3 from p3.vfat
make_card() {
	rm -f "$1"
	truncate -s "$2" "$1"
	sfdisk -q "$1" 2>/dev/null <<-EOF
	label: dos
	start=32768, size=65536, type=c, bootable
	start=98304, size=32768, type=83
	start=$P3, size=$P3, type=c
	EOF
	dd if=p3.vfat of="$1" bs=1M seek=64 conv=notrunc status=none
	dd if="$1" of="$1.orig" bs=1M count=128 status=none
}

# run NAME EXPECTED_EXIT [ENV...]: the grow, its log in NAME.log
run() {
	name=$1
	want=$2
	shift 2
	rc=0
	env "$@" "$QEMU" "$FE" --sdgrow "$name" "$name.d" "$SELF" \
		>>"$name.log" 2>&1 || rc=$?
	[ $rc = "$want" ] || { cat "$name.log"; fail "$name: exit $rc, expected $want"; }
}

has() {
	grep -q "$2" "$1.log" || { cat "$1.log"; fail "$1: no '$2' in the log"; }
}

p3_size() {
	sfdisk -d "$1" 2>/dev/null | sed -n 's/.*3 : start= *[0-9]*, size= *\([0-9]*\).*/\1/p'
}

# The card is the original again.
same_as_orig() {
	dd if="$1" bs=1M count=128 status=none | cmp -s - "$1.orig" ||
		fail "$1: not restored to the original"
}

# check_card NAME CLUSTER_KIB: p3 to the 4 MiB aligned end, a valid FAT32
# with the files of src.
check_card() {
	sectors=$(($(stat -c %s "$1") / 512))
	[ $sectors -gt 4294967295 ] && sectors=4294967295
	want=$((sectors / 8192 * 8192 - P3))
	size=$(p3_size "$1")
	[ "$size" = $want ] || fail "$1: p3 has $size sectors, expected $want"
	sfdisk -d "$1" 2>/dev/null | grep -q "3 : start= *$P3, size= *$size, type=c" ||
		fail "$1: p3 entry"
	sfdisk -d "$1" 2>/dev/null | grep -q "1 : start= *32768, size= *65536, type=c, bootable" ||
		fail "$1: p1 entry"
	# The file system: its first 1100 MiB (all metadata and files, even
	# the FATs of 2 TiB), the rest is unused.
	rm -f "$1.p3"
	dd if="$1" of="$1.p3" bs=1M skip=64 count=1100 conv=sparse status=none
	truncate -s $((size * 512)) "$1.p3"
	fsck.fat -n "$1.p3" > "$1.fsck" 2>&1 || { cat "$1.fsck"; fail "$1: fsck.fat"; }
	file "$1.p3" | grep -q 'FAT (32 bit)' || fail "$1: not FAT32: $(file "$1.p3")"
	file "$1.p3" | grep -q 'label: "GBA        "' || fail "$1: label"
	file "$1.p3" | grep -q "sectors/cluster $(($2 * 2))," ||
		fail "$1: not $2 KiB clusters: $(file "$1.p3")"
	minfo -i "$1.p3" > "$1.minfo"
	grep -q 'disk label="GBA        "' "$1.minfo" || fail "$1: minfo label"
	rm -rf "$1.out"
	mkdir "$1.out"
	if [ "$3" != empty ]; then
		mcopy -i "$1.p3" -s -p -m -n "::/*" "$1.out/"
		diff -r src "$1.out" || fail "$1: files differ"
		(cd src && find . -type f -printf '%p %s %T@\n' | sort) > "$1.want"
		(cd "$1.out" && find . -type f -printf '%p %s %T@\n' | sort) > "$1.got"
		cmp -s "$1.want" "$1.got" || { diff "$1.want" "$1.got"; fail "$1: mtimes"; }
	else
		[ -z "$(ls -A "$1.out")" ] || fail "$1: not empty"
	fi
	rm -rf "$1.p3" "$1.out" "$1.d"
}

# A first boot, and the next one finds nothing to do.
make_card plain 1G
run plain 0
has plain 'sdgrow: copied 157 files in 7 directories'
has plain 'sdgrow: new p3: FAT32, 4 KiB clusters'
has plain 'sdgrow: checked 157 files'
has plain 'sdgrow: done: p3 grown from 64 MiB to 960 MiB'
check_card plain 4
cp plain plain.grown
run plain 0
has plain 'fills the card (1024 MiB): nothing to do'
cmp -s plain plain.grown || fail "plain: changed by the second run"
rm -rf plain plain.*

# Power loss at every step: the next boot ends with the same result.
for step in copied backup header mbr format-half format copy-in verified; do
	make_card "cut-$step" 1G
	run "cut-$step" 3 FE_SDGROW_STOP=$step
	has "cut-$step" "stopping at $step"
	run "cut-$step" 0
	has "cut-$step" 'sdgrow: attempt 2: '
	case $step in
	copied|backup|header)
		has "cut-$step" 'interrupted before the MBR change'
		! grep -q 'restore:' "cut-$step.log" || fail "$step: restored";;
	*)
		has "cut-$step" 'interrupted after the MBR change'
		has "cut-$step" 'restore: old p3 back (64 MiB at 64 MiB)';;
	esac
	has "cut-$step" 'sdgrow: done'
	check_card "cut-$step" 4
	rm -rf "cut-$step" "cut-$step".*
done

# An interrupted restore is done again.
make_card cut-restore 1G
run cut-restore 3 FE_SDGROW_STOP=format
run cut-restore 3 FE_SDGROW_STOP=restore-half
run cut-restore 0
has cut-restore 'sdgrow: attempt 2: '
has cut-restore 'sdgrow: done'
check_card cut-restore 4
rm -rf cut-restore cut-restore.*

# A failed check in the same boot: the old p3 back, never again.
make_card failed 1G
run failed 2 FE_SDGROW_FAIL=verify
has failed 'the check failed (I/O error), restoring the old p3'
has failed 'restore: old p3 back (64 MiB at 64 MiB), not trying again'
same_as_orig failed
run failed 2
has failed 'gave up in attempt 1 (the check), not growing'
same_as_orig failed
rm -rf failed failed.*

# Interrupted three times after the MBR change: restored, then left.
make_card limit 1G
run limit 3 FE_SDGROW_STOP=format
run limit 3 FE_SDGROW_STOP=format
run limit 3 FE_SDGROW_STOP=format
run limit 2
has limit 'restore: old p3 back (64 MiB at 64 MiB), not trying again'
has limit '3 attempts interrupted, not trying again'
same_as_orig limit
run limit 2
has limit 'gave up in attempt 3 (3 interruptions)'
rm -rf limit limit.*

# A p3 that is not a file system is left alone, a blank one formatted.
make_card garbage 1G
head -c 1048576 /dev/urandom | dd of=garbage bs=1M seek=64 conv=notrunc status=none
dd if=garbage of=garbage.orig bs=1M count=128 status=none
run garbage 1
has garbage 'does not mount (I/O error) and is not blank, leaving it alone'
same_as_orig garbage
[ "$(p3_size garbage)" = $P3 ] || fail "garbage: p3 changed"
rm -rf garbage garbage.*

make_card blank 1G
dd if=/dev/zero of=blank bs=1M seek=64 count=64 conv=notrunc status=none
run blank 0
has blank 'p3 is blank'
check_card blank 4 empty
rm -rf blank blank.*

# 32 KiB clusters above 32 GiB; the MBR's 2 TiB limit.
make_card big 40G
run big 0
check_card big 32
rm -rf big big.*
make_card huge 3T
run huge 0
has huge 'growing it to 2097084 MiB'
check_card huge 32
rm -rf huge huge.*

cd /
rm -rf "$DIR"
echo "sdgrow-test: passed"
