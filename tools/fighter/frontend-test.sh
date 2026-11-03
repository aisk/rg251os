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
# The sound banks sit next to the packs and stay out of the ROM list.
cp "${content%.fight}.fightsnd" "$data/roms/fighters/Fighter-POC.fightsnd"
cp "${automatic%.fight}.fightsnd" "$data/roms/fighters/Fighter-Auto.fightsnd"
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
grep -q 'fight: FIGHTP10 package .* characters' "$f"
grep -q 'fight: stage 0 KOREA width 752' "$f"
grep -q 'fight: resident KiB frame indices' "$f"
grep -q 'fight: sound bank .*/Fighter-POC.fightsnd .* SFX .* tracks' "$f"
# The select screen is confirmed before P1's fill completes, so the first
# completed fill is the match's, spread over the versus card.
grep -q 'fight: cast TERRY palette 0 vs [A-Z ]* palette [01] on [A-Z]* ready in [0-9]* runs (fight): expanded .* casts superseded; resident RGB565 KiB' "$f"
grep -q 'state: wrote [0-9]* bytes to Fighter-POC.fight.fighter.state' "$f"
grep -q 'grid off .*color off' "$f"
! test -e "$data/saves/Fighter-POC.fight.sav"
FE_SLEEP_AT=1500 run "$rom" 1800
grep -q 'state: loaded [0-9]* bytes from Fighter-POC.fight.fighter.state' "$f"
grep -q 'fight: state loaded mid-fight, blocking cache fill [0-9]* KiB in [0-9]* us' "$f"
grep -q 'sleep #1: awake (light sleep)' "$f"
grep -q 'underruns after warmup 0' "$f"
FE_RESET_AT=60 run "$rom" 120
grep -q 'headless: restart at frame 60 done' "$f"
grep -q 'state: removed Fighter-POC.fight.fighter.state (restart game)' "$f"
FE_END=poweroff run "$rom" 120
grep -q '^core=fighter$' "$data/config/resume.txt"
FE_END=quit run resume 120
grep -q 'resume: found fighters/Fighter-POC.fight with fighter' "$f"
grep -q 'state: loaded [0-9]* bytes from Fighter-POC.fight.fighter.state' "$f"
grep -q 'headless: quit to ROM list' "$f"
grep -q 'roms: folder /fighters: 2 ROMs, 0 folders' "$f"
test ! -e "$data/config/resume.txt"
# One full pressure cycle and the start of the next, with debug still off.
run "$data/roms/fighters/Fighter-Auto.fight" 2500
for scene in 0 1 2 3; do
    grep -q "fight: scene $scene render us avg/p95/p99/max" "$f"
    grep -q "fight: scene $scene cache us avg/p95/p99/max" "$f"
    grep -q "fight: scene $scene effects drawn/requested .* sparks [0-9]*, .*stage [A-Z]* cam [0-9]*" "$f"
done
grep -q 'fight: TERRY stand LP startup/active/recovery 3/3/6' "$f"
grep -q 'fight: TERRY special 236+3 startup/active/recovery 0/0/41' "$f"
grep -q 'fight: tick .* specials .* run .* stage' "$f"
grep -q 'fight: cast .* vs .* on [A-Z]* ready in [0-9]* runs (fight)' "$f"
grep -q 'fight: scene 0 sound starts [1-9].* music [a-z]*-[a-z0-9]* .* MP2 frames [1-9]' "$f"
cp "$f" "$data/log/automatic.txt"
printf 'broken package\n' > "$data/roms/fighters/Bad.fight"
if run "$data/roms/fighters/Bad.fight" 1; then
    echo 'fighter frontend test: malformed content was accepted' >&2
    exit 1
fi
grep -q 'fight: invalid FIGHTP10 package' "$f"
grep -q 'headless: .*Bad.fight did not start' "$f"
test ! -e "$data/saves/Bad.fight.fighter.state"
echo 'fighter frontend test: load, cast, sound bank, state, sleep, restart, boot resume, list, four scenes, ladder log and bad content passed'
