#!/bin/sh
# Exercise the real fighter through the frontend's lifecycle under qemu-user.
# All paths come from make; logs stay in build/ for failure diagnosis.
set -eu
qemu=$1
frontend=$2
content=$3
automatic=$4
data=$5
mkdir -p "$data/roms/fighters" "$data/config/roms"
cp "$content" "$data/roms/fighters/Fighter-POC.fight"
cp "$automatic" "$data/roms/fighters/Fighter-Auto.fight"
f="$data/log/frontend.txt"
rom="$data/roms/fighters/Fighter-POC.fight"
run() { "$qemu" "$frontend" --headless "$data" "$@"; }
# A conflicting GBA selection/effects must not affect content dispatch.
printf 'core=mgba\ncolor=gba\nframeskip=2\n' > "$data/config/roms/Fighter-POC.fight.txt"
run "$rom" 120
cp "$f" "$data/log/first.txt"
grep -q 'starting .*Fighter-POC.fight with fighter' "$f"
grep -q 'fight: pacing one tick per vblank' "$f"
grep -q 'frame 480x320 pitch 480' "$f"
grep -q 'state: wrote 184 bytes to Fighter-POC.fight.fighter.state' "$f"
grep -q 'grid off .*color off' "$f"
! test -e "$data/saves/Fighter-POC.fight.sav"
FE_SLEEP_AT=1500 run "$rom" 1800
grep -q 'state: loaded 184 bytes from Fighter-POC.fight.fighter.state' "$f"
grep -q 'sleep #1: awake (light sleep)' "$f"
grep -q 'underruns after warmup 0' "$f"
FE_RESET_AT=60 run "$rom" 120
grep -q 'headless: restart at frame 60 done' "$f"
grep -q 'state: removed Fighter-POC.fight.fighter.state (restart game)' "$f"
FE_END=poweroff run "$rom" 120
grep -q '^core=fighter$' "$data/config/resume.txt"
FE_END=quit run resume 120
grep -q 'resume: found fighters/Fighter-POC.fight with fighter' "$f"
grep -q 'state: loaded 184 bytes from Fighter-POC.fight.fighter.state' "$f"
grep -q 'headless: quit to ROM list' "$f"
grep -q 'roms: folder /fighters: 2 ROMs, 0 folders' "$f"
test ! -e "$data/config/resume.txt"
# One full pressure cycle and the start of the next, with debug still off.
run "$data/roms/fighters/Fighter-Auto.fight" 2500
for scene in 0 1 2 3; do
    grep -q "fight: scene $scene render us avg/p95/p99/max" "$f"
done
cp "$f" "$data/log/automatic.txt"
printf 'broken package\n' > "$data/roms/fighters/Bad.fight"
if run "$data/roms/fighters/Bad.fight" 1; then
    echo 'fighter frontend test: malformed content was accepted' >&2
    exit 1
fi
grep -q 'fight: invalid POC package' "$f"
grep -q 'headless: .*Bad.fight did not start' "$f"
test ! -e "$data/saves/Bad.fight.fighter.state"
echo 'fighter frontend test: load, state, sleep, restart, boot resume, list, four scenes and bad content passed'
