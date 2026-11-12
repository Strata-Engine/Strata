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
      STRATA_SHORE=1 STRATA_COMBAT=1 STRATA_LIGHT=0 STRATA_LEVELS=0 STRATA_SHOT=/dev/null "$@" \
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
  fl=$(num duel '^combat: duel flinches \([0-9]*\), .*$'); fa=$(num duel '^combat: duel flinches [0-9]*, falls \([0-9]*\), .*$')
  most=$(num duel '^combat: duel .*, pieces at most \([0-9]*\), .*$'); now=$(num duel '^combat: duel .*, pieces now \([0-9]*\), .*$'); rigs=$(num duel '^combat: duel .*, rigs \([0-9]*\)$')
  if [ "$a" -ge 0 ] && [ $((a + b)) -ge 1 ] && [ "$fl" -ge 1 ] && [ "$fa" -ge 1 ] && [ "$most" -ge 15 ] && [ "$most" -le $((15 * fa)) ] && [ "$rigs" -eq 12 ] && [ "$(num duel '^combat: duel arrows stuck \([0-9]*\), upright [0-9]*, .*$')" -ge 1 ] && [ $(( $(num duel '^combat: duel arrows stuck [0-9]*, upright \([0-9]*\), .*$') * 2 )) -le "$(num duel '^combat: duel arrows stuck \([0-9]*\), upright [0-9]*, .*$')" ]; then echo "ok   duel: kills $a and $b; $fl flinches, $fa falls, $most loose pieces at most ($now left), 12 shirts"
  else echo "FAIL duel: kills $a and $b (want one at least), flinches $fl, falls $fa (want one of each), pieces at most $most (want 15 to 15 a fall), rigs $rigs (want 12), or its stuck arrows stand upright"; cat "$OUT/duel.txt"; FAILED=1; fi
fi
# A trap: the ground dug from under a rusher, a block set where a shooter stands. The rusher falls into the pit; no one ends inside a block; the idle rule frees the stuck.
if twice trap STRATA_COMBAT_SCENE=trap STRATA_FRAMES=1800; then
  fell=$(num trap '^combat: trap fell \([0-9]*\) cm, .*$'); at420=$(num trap '^combat: trap fell .*, inside at frame 420 \([0-9]*\), .*$')
  inside=$(num trap '^combat: trap fell .*, inside \([0-9]*\), respawns [0-9]*$'); back=$(num trap '^combat: trap fell .*, respawns \([0-9]*\)$')
  if [ "$fell" -ge 250 ] && [ "$inside" -eq 0 ] && [ "$back" -ge 1 ]; then echo "ok   trap: the rusher fell $fell cm into the pit; $at420 inside a block 2 s after it was set, none at the end; $back put back by the idle rule"
  else echo "FAIL trap: fell $fell cm (want 250 or more), inside at the end $inside (want 0), respawns $back (want 1 or more)"; cat "$OUT/trap.txt"; FAILED=1; fi
fi
# Fire: a fireball thrown at a tree's trunk burns wood and grass within 2 m and hurts the shooter standing a metre off by about 40.
if twice fire STRATA_COMBAT_SCENE=fire STRATA_FRAMES=200; then
  wood=$(num fire '^combat: fire burned [0-9]* cells: wood \([0-9]*\), .*$'); grass=$(num fire '^combat: fire burned .*, grass \([0-9]*\), .*$'); left=$(num fire '^combat: fire burned .*, bystander health \([0-9]*\)$')
  if [ "$wood" -ge 1 ] && [ "$grass" -ge 1 ] && [ "$left" -ge 10 ] && [ "$left" -le 35 ]; then echo "ok   fire: burned $wood wood and $grass grass; the bystander is left with $left of 60"
  else echo "FAIL fire: wood $wood, grass $grass (want 1 or more of each), bystander left with $left (want 10 to 35)"; cat "$OUT/fire.txt"; FAILED=1; fi
fi
# Water: a rusher swims a 6 m channel to its target, at the surface, climbs out and strikes.
if twice channel STRATA_COMBAT_SCENE=channel STRATA_FRAMES=1800; then
  swam=$(num channel '^combat: channel swam \([0-9]*\) frames, .*$'); crossed=$(num channel '^combat: channel swam .*, crossed \([0-9]*\), .*$')
  struck=$(num channel '^combat: channel swam .*, struck \([0-9]*\), .*$'); deep=$(num channel '^combat: channel swam .*, deepest \([0-9]*\) cm$')
  if [ "$swam" -gt 60 ] && [ "$crossed" -eq 1 ] && [ "$struck" -ge 1 ] && [ "$deep" -le 120 ]; then echo "ok   channel: swam $swam frames no deeper than $deep cm, crossed, struck $struck times"
  else echo "FAIL channel: swam $swam frames (want over 60), crossed $crossed (want 1), struck $struck (want 1 or more), deepest $deep cm (want 120 or less)"; cat "$OUT/channel.txt"; FAILED=1; fi
fi
# The player, by script: an archer fells a still rusher 20 m off with three arrows.
if twice archer STRATA_COMBAT_SCRIPT=archer STRATA_FRAMES=150; then
  shots=$(num archer '^combat: archer shots \([0-9]*\), kills [0-9]*, .*$'); k=$(num archer '^combat: archer shots [0-9]*, kills \([0-9]*\), .*$')
  pushes=$(num archer '^combat: archer shots .*, pushes \([0-9]*\)$')
  if [ "$shots" -eq 3 ] && [ "$k" -eq 1 ] && [ "$pushes" -eq 2 ]; then echo "ok   archer: three arrows, one kill, the two it survived pushed it back"
  else echo "FAIL archer: shots $shots (want 3), kills $k (want 1), pushes $pushes (want 2)"; cat "$OUT/archer.txt"; FAILED=1; fi
fi
# The same with a stone wall a metre behind the target: an arrow that passes through it into the wall in one tick still counts.
if twice waller STRATA_COMBAT_SCRIPT=waller STRATA_FRAMES=150; then
  shots=$(num waller '^combat: archer shots \([0-9]*\), kills [0-9]*, .*$'); k=$(num waller '^combat: archer shots [0-9]*, kills \([0-9]*\), .*$')
  if [ "$shots" -eq 3 ] && [ "$k" -eq 1 ]; then echo "ok   waller: three arrows fell a target backed by a wall"
  else echo "FAIL waller: shots $shots (want 3), kills $k (want 1)"; cat "$OUT/waller.txt"; FAILED=1; fi
fi
# A player who only stands is killed by a rusher, lies 3 s, and is stood at a spawn point by the viewer.
if twice victim STRATA_COMBAT_SCRIPT=victim STRATA_FRAMES=700; then
  died=$(num victim '^combat: player died at frame \([0-9]*\), back at frame [0-9]*$'); back=$(num victim '^combat: player died at frame [0-9]*, back at frame \([0-9]*\)$')
  off=$(num victim '^combat: victim eye \([0-9]*\) cm from its spawn point, deaths [0-9]*$')
  if [ "$died" -gt 0 ] && [ $((back - died)) -ge 179 ] && [ $((back - died)) -le 181 ] && [ "$off" -ge 0 ] && [ "$off" -le 100 ]; then echo "ok   victim: died at frame $died, back $((back - died)) frames later, $off cm from its spawn point"
  else echo "FAIL victim: died $died, back $back (want 180 frames later), eye $off cm from its spawn point (want 100 or less)"; cat "$OUT/victim.txt"; FAILED=1; fi
fi
# A player far from the field pauses the match: nothing changes after frame 60.
if twice far STRATA_COMBAT_SCRIPT=far STRATA_FRAMES=600; then
  h60=$(num far '^combat: far fingerprint \([0-9]*\) at frame 60$'); h600=$(num far '^combat: frame 600 fingerprint \([0-9]*\), .*$')
  if [ "$h60" -ge 0 ] && [ "$h60" = "$h600" ]; then echo "ok   far: the match stood still from frame 60 to 600"
  else echo "FAIL far: fingerprint $h60 at frame 60, $h600 at 600 (want equal)"; cat "$OUT/far.txt"; FAILED=1; fi
fi
# Ten short matches in a row: each has its winner, and nothing of an earlier match is left in the world.
if twice ten STRATA_COMBAT_PLAYER=0 STRATA_COMBAT_WIN=3 STRATA_COMBAT_REPEAT=9 STRATA_FRAMES=90000; then
  wins=$(grep -c '^combat: winner [A-Za-z]* 3 at frame' "$OUT/ten.txt"); ents=$(num ten '^combat: matches 10, live entities \([0-9]*\), figures [0-9]*$'); figs=$(num ten '^combat: matches 10, live entities [0-9]*, figures \([0-9]*\)$')
  starts=$(sed -n 's/^combat: rematch [0-9]*: \(.*\)$/\1/p' "$OUT/ten.txt" | sort -u | wc -l | tr -d ' '); first=$(sed -n 's/^combat: rematch [0-9]*: \(.*\)$/\1/p' "$OUT/ten.txt" | head -1)
  if [ "$wins" -eq 10 ] && [ "$(grep -c '^combat: rematch ' "$OUT/ten.txt")" -eq 9 ] && [ "$starts" -eq 1 ] && [ "$first" = "entities added 11, figures 15" ]; then echo "ok   ten matches: ten winners; each rematch begins with 11 entities and 15 figures, nothing left over"
  else echo "FAIL ten matches: $wins winners (want 10); rematches begin with $starts different counts (want 1: entities added 11, figures 15), the first: $first"; cat "$OUT/ten.txt"; FAILED=1; fi
fi
# The match with no player: someone reaches 30, and the same someone the second time.
if twice match STRATA_COMBAT_PLAYER=0 STRATA_FRAMES=90000; then
  if grep -q '^combat: winner [A-Za-z]* 30 at frame [0-9]*$' "$OUT/match.txt" && [ "$(grep -c '^combat: table ' "$OUT/match.txt")" -eq 12 ]; then echo "ok   match: $(grep '^combat: winner' "$OUT/match.txt" | sed 's/^combat: //')"
  else echo "FAIL match: no winner in 90000 frames"; cat "$OUT/match.txt"; FAILED=1; fi
fi

[ $FAILED -eq 0 ] && echo "combat: PASS" || { echo "combat: FAIL (logs in $OUT)"; exit 1; }
