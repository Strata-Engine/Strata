# Head-related impulse responses

`kemar/` holds the compact set of the MIT KEMAR measurements: 368 stereo
WAV files, 128 samples at 44.1 kHz, left and right ear, for sources at
elevations -40 to +90 degrees and azimuths 0 to 180 degrees (clockwise from
straight ahead; 90 is to the right). The left half is the mirror. The
responses are equalised for the measuring loudspeaker.

Source: https://sound.media.mit.edu/resources/KEMAR.html, `compact.zip`
(sha256 0bcd69f8e8760cf8eacff4a89594ceca7d44fb94f28ecf0bf19970003bbfb3e0),
fetched 2026-11-11.

Citation, as the authors ask:

> Bill Gardner and Keith Martin, "HRTF Measurements of a KEMAR Dummy-Head
> Microphone", MIT Media Lab Perceptual Computing Technical Report #280,
> May 1994.

Their terms: "This data is Copyright 1994 by the MIT Media Laboratory. It is
provided free with no restrictions on use, provided the authors are cited
when the data is used in any research or commercial application."

`native/tools/make_hrir.c` reads these files and writes
`native/strata_hrir.h`, the binaural decoder's filters; run
`native/stamp_audio.sh` after regenerating it (GAPS S14).
