#!/bin/sh
# The match's scripted checks: hidden runs of the viewer with STRATA_COMBAT=1,
# each twice, read from the `combat:` lines it prints. Needs a GPU, and the
# viewer's release build (verify.sh builds it first; `--build` builds it here).
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
if [ "$1" = "--build" ]; then (cd viewer && forge build --release) > /dev/null 2>&1 || { echo "FAIL: viewer build"; exit 1; }; fi
OUT="${TMPDIR:-/tmp}/strata-combat.$$"; mkdir -p "$OUT"
FAILED=0
# run NAME ENV... : the run's combat lines in $OUT/NAME.txt
run() {
  name="$1"; shift
  env MARCH_PIN_MAIN=1 STRATA_HIDDEN=1 STRATA_OFFSCREEN=1 STRATA_PIXELS=1 STRATA_AUDIO=0 STRATA_SIZE=640x400 STRATA_ISLAND=1 \
      STRATA_SHORE=1 STRATA_COMBAT=1 STRATA_LIGHT=0 STRATA_SHOT=/dev/null "$@" \
      ./viewer/.march/build/release/strata_viewer > "$OUT/$name.log" 2>&1
  grep '^combat:' "$OUT/$name.log" > "$OUT/$name.txt"
}
# twice NAME ENV... : two runs that must print the same combat lines
twice() {
  both="$1"; shift
  run "$both-a" "$@"; run "$both-b" "$@"
  if ! cmp -s "$OUT/$both-a.txt" "$OUT/$both-b.txt"; then echo "FAIL $both: two runs differ"; diff "$OUT/$both-a.txt" "$OUT/$both-b.txt" | head -6; FAILED=1; return 1; fi
  cp "$OUT/$both-a.txt" "$OUT/$both.txt"
}
# num NAME 'sed pattern with one group' : the number, or -1
num() { v=$(sed -n "s/$2/\1/p" "$OUT/$1.txt" | head -1); echo "${v:--1}"; }

# The field and the fighters: they stand on dry ground about a middle and walk.
if twice field STRATA_COMBAT_PLAYER=0 STRATA_FRAMES=900; then
  dry=$(num field '^combat: 12 fighters, 16 spawn points, middle (.*), dry \([0-9]*\) of 64$')
  moved=$(num field '^combat: frame 900 fingerprint [0-9]*, moved \([0-9]*\) m, outside [0-9]*$')
  outside=$(num field '^combat: frame 900 fingerprint [0-9]*, moved [0-9]* m, outside \([0-9]*\)$')
  if [ "$dry" -ge 52 ] && [ "$moved" -gt 100 ] && [ "$outside" -eq 0 ]; then echo "ok   field: dry $dry of 64, fighters walked $moved m, none off the field"
  else echo "FAIL field: dry $dry (want 52 or more), moved $moved m (want over 100), outside $outside (want 0)"; cat "$OUT/field.txt"; tail -3 "$OUT/field-a.log"; FAILED=1; fi
fi

# A duel: two shooters 15 m apart loose arrows until one falls.
if twice duel STRATA_COMBAT_SCENE=duel STRATA_FRAMES=1200; then
  a=$(num duel '^combat: duel kills \([0-9]*\) [0-9]*$'); b=$(num duel '^combat: duel kills [0-9]* \([0-9]*\)$')
  if [ "$a" -ge 0 ] && [ $((a + b)) -ge 1 ]; then echo "ok   duel: kills $a and $b"
  else echo "FAIL duel: kills $a and $b (want one at least)"; cat "$OUT/duel.txt"; FAILED=1; fi
fi
# A trap: the ground dug from under a rusher, a block set where a shooter stands. No one ends inside a block, and the idle rule frees the stuck.
if twice trap STRATA_COMBAT_SCENE=trap STRATA_FRAMES=1800; then
  inside=$(num trap '^combat: trap inside \([0-9]*\), respawns [0-9]*$'); back=$(num trap '^combat: trap inside [0-9]*, respawns \([0-9]*\)$')
  if [ "$inside" -eq 0 ] && [ "$back" -ge 1 ]; then echo "ok   trap: no one inside a block, $back put back by the idle rule"
  else echo "FAIL trap: inside $inside (want 0), respawns $back (want 1 or more)"; cat "$OUT/trap.txt"; FAILED=1; fi
fi
# Fire: a fireball thrown at a tree's trunk burns wood and grass within 2 m and hurts the shooter standing a metre off by about 40.
if twice fire STRATA_COMBAT_SCENE=fire STRATA_FRAMES=200; then
  wood=$(num fire '^combat: fire burned [0-9]* cells: wood \([0-9]*\), .*$'); grass=$(num fire '^combat: fire burned .*, grass \([0-9]*\), .*$'); left=$(num fire '^combat: fire burned .*, bystander health \([0-9]*\)$')
  if [ "$wood" -ge 1 ] && [ "$grass" -ge 1 ] && [ "$left" -ge 10 ] && [ "$left" -le 35 ]; then echo "ok   fire: burned $wood wood and $grass grass; the bystander is left with $left of 60"
  else echo "FAIL fire: wood $wood, grass $grass (want 1 or more of each), bystander left with $left (want 10 to 35)"; cat "$OUT/fire.txt"; FAILED=1; fi
fi
# The match with no player: someone reaches 30, and the same someone the second time.
if twice match STRATA_COMBAT_PLAYER=0 STRATA_FRAMES=90000; then
  if grep -q '^combat: winner [A-Za-z]* 30 at frame [0-9]*$' "$OUT/match.txt" && [ "$(grep -c '^combat: table ' "$OUT/match.txt")" -eq 12 ]; then echo "ok   match: $(grep '^combat: winner' "$OUT/match.txt" | sed 's/^combat: //')"
  else echo "FAIL match: no winner in 90000 frames"; cat "$OUT/match.txt"; FAILED=1; fi
fi

[ $FAILED -eq 0 ] && echo "combat: PASS" || { echo "combat: FAIL (logs in $OUT)"; exit 1; }
