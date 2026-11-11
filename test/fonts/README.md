Subsets of Noto fonts, each cut down to the characters the tests and the
golden scenes draw (`pyftsubset --layout-features='*' --no-hinting
--notdef-outline`). Noto is licensed under the SIL Open Font Licence 1.1:
see OFL.txt. The gate reads only these; it never reads a system font.

- NotoSans-Latin.ttf: Basic Latin, Latin-1, a little punctuation. Also
  embedded in the shim as the default font (native/strata_font.h).
- NotoSerif-Latin.ttf: Basic Latin.
- NotoSansArabic-Sub.ttf, NotoSansHebrew-Sub.ttf,
  NotoSansDevanagari-Sub.ttf, NotoSansSC-Sub.otf, NotoSansJP-Sub.otf,
  NotoSansKR-Sub.otf: the characters of the script lines only.
