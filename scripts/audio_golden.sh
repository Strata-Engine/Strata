#!/bin/sh
# Render soundcheck's scenes in game time (no device, nothing played aloud)
# and compare each one's fingerprint with test/golden-audio/<scene>.fp.
# `scripts/audio_golden.sh --update` stores new fingerprints instead and
# keeps the WAVs in $TMPDIR/strata-audio-listen: listen before committing.
cd "$(dirname "$0")/.." || exit 1
. scripts/env.sh
native/stamp_audio.sh --check || exit 1
UPDATE=0; [ "$1" = "--update" ] && UPDATE=1
OUT="${TMPDIR:-/tmp}/strata-audio.$$"; mkdir -p "$OUT" test/golden-audio native/tests/bin
trap 'rm -rf "$OUT"' EXIT
LISTEN="${TMPDIR:-/tmp}/strata-audio-listen"

cc -O2 -Wall -Wextra -Inative -o native/tests/bin/fp_diff native/tests/fp_diff.c || exit 1
cc -O2 -Wall -Wextra -Inative -o native/tests/bin/wav_fp native/tests/wav_fp.c || exit 1
(cd soundcheck && forge build --release > "$OUT/build.log" 2>&1) || { echo "FAIL: soundcheck build"; tail -20 "$OUT/build.log"; exit 1; }
SC=soundcheck/.march/build/release/soundcheck

# no device: open fails, the game goes on, every record is counted
STRATA_AUDIO_FAIL=1 SOUNDCHECK_SCENE=nodevice $SC > "$OUT/nodevice.log" 2>&1 || { echo "FAIL: nodevice"; cat "$OUT/nodevice.log"; exit 1; }

# what an acoustics update costs (reported, not judged here)
SOUNDCHECK_SCENE=acoustics64 $SC > "$OUT/acoustics64.log" 2>&1 || { echo "FAIL: acoustics64"; cat "$OUT/acoustics64.log"; exit 1; }
cat "$OUT/acoustics64.log"

# a decoded budget refuses a recording past it, and counts it
SOUNDCHECK_SCENE=budget SOUNDCHECK_OUT="$OUT/budget.wav" $SC > "$OUT/budget.log" 2>&1 || { echo "FAIL: budget"; cat "$OUT/budget.log"; exit 1; }

SCENES='blocks strike tone recording orbit-speakers orbit-headphones distance load64 wall cave field falls-4 falls-12 falls-24'
fail=0
[ $UPDATE -eq 1 ] && mkdir -p "$LISTEN"
for s in $SCENES; do
  SOUNDCHECK_SCENE=$s SOUNDCHECK_OUT="$OUT/$s.wav" $SC > "$OUT/$s.log" 2>&1 || { echo "FAIL: $s did not render"; cat "$OUT/$s.log"; exit 1; }
  native/tests/bin/wav_fp "$OUT/$s.wav" "$OUT/$s.fp" > /dev/null || { echo "FAIL: $s fingerprint"; exit 1; }
  if [ $UPDATE -eq 1 ]; then
    cp "$OUT/$s.fp" "test/golden-audio/$s.fp"; cp "$OUT/$s.wav" "$LISTEN/$s.wav"
    echo "stored $s; listen: $LISTEN/$s.wav"
  elif [ ! -f "test/golden-audio/$s.fp" ]; then
    echo "FAIL: no stored fingerprint for $s (scripts/audio_golden.sh --update, then listen)"; fail=1
  elif native/tests/bin/fp_diff "test/golden-audio/$s.fp" "$OUT/$s.fp" 2> "$OUT/$s.diff"; then
    echo "ok $s"
  else
    echo "FAIL: $s differs from its fingerprint"; head -8 "$OUT/$s.diff"; fail=1
  fi
done
# a recording of the tone, loaded back and played, sounds as the tone did
if [ $UPDATE -eq 0 ] && ! native/tests/bin/fp_diff "$OUT/tone.fp" "$OUT/recording.fp" 2> "$OUT/rec.diff"; then
  echo "FAIL: the tone played back from a file differs from the tone"; head -8 "$OUT/rec.diff"; fail=1
fi
# the cave rings: in the half second after each strike its tail is 10 dB above the field's
tail_db() { awk 'NR > 1 { w = NR - 2; t = w % 10; if (t >= 4 && t <= 8) { for (i = 1; i <= 20; i++) e += 10 ^ ($i / 10); n += 20 } } END { printf "%.1f", 10 * log(e / n) / log(10) }' "$1"; }
if [ $UPDATE -eq 0 ]; then
  c=$(tail_db "$OUT/cave.fp"); f=$(tail_db "$OUT/field.fp")
  if awk -v c="$c" -v f="$f" 'BEGIN { exit !(c - f >= 10) }'; then echo "ok cave rings: tail $c dB against the field's $f"
  else echo "FAIL: the cave's tail ($c dB) is not 10 dB above the field's ($f dB)"; fail=1; fi
fi
# falling water: louder and brighter the higher it falls
falls() { awk 'NR > 11 { for (i = 1; i <= 20; i++) e += 10 ^ ($i / 10); hi += 10 ^ ($8 / 10) + 10 ^ ($18 / 10); lo += 10 ^ ($5 / 10) + 10 ^ ($15 / 10) } END { printf "%.2f %.2f", 10 * log(e) / log(10), 10 * log(hi / lo) / log(10) }' "$1"; }
if [ $UPDATE -eq 0 ]; then
  a=$(falls "$OUT/falls-4.fp"); b=$(falls "$OUT/falls-12.fp"); c=$(falls "$OUT/falls-24.fp")
  if echo "$a $b $c" | awk '{ exit !($1 < $3 && $3 < $5 && $2 < $4 && $4 < $6) }'; then echo "ok falls: level and brightness rise with height ($a / $b / $c)"
  else echo "FAIL: falling water is not louder and brighter the higher it falls ($a / $b / $c)"; fail=1; fi
fi
[ $fail -eq 0 ] && echo "audio goldens: all match"
exit $fail
