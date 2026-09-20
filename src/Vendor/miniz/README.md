# miniz

Vendored third-party code. Not covered by the project's own style rules, and excluded from
`editorconfig-checker` (`.ecrc`) and `cspell` (`cspell.json`) for that reason.

| | |
|---|---|
| Upstream | <https://github.com/richgel999/miniz> |
| Version | 3.0.2 |
| Source | the `miniz-3.0.2.zip` release asset of <https://github.com/richgel999/miniz/releases/tag/3.0.2> |
| License | MIT, see `LICENSE` |

`miniz.c` and `miniz.h` are the amalgamated build from that asset, byte for byte. `.gitattributes`
pins `src/Vendor/**` to `eol=lf` so they stay that way on a Windows checkout. To check:

```sh
curl -sL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip -o miniz.zip
unzip -p miniz.zip miniz.c | diff - src/Vendor/miniz/miniz.c
unzip -p miniz.zip miniz.h | diff - src/Vendor/miniz/miniz.h
```

```
sha256  0fcdc9888cb3a29ca8f176bac087e5fe6c7258a6ab06b1c271c1e109a11d3740  miniz.c
sha256  295d1a0041aea09609598c0f1f35c1977ca05ad662acbadcfdaac44c140af37b  miniz.h
```

Keep it that way. Fix bugs by taking a newer upstream release, never by editing these two files.

## What is compiled

Only raw deflate and inflate, for the replay frame stream in `src/Replay/ReplayStream.cpp`. The
ZIP, PNG and zlib-compatibility layers are switched off by the `MINIZ_NO_*` defines on the
`miniz.c` entry in `Spawner.vcxproj`; nothing outside `ReplayStream.cpp` includes this header.

Splitting the upstream sources instead of taking the amalgamation was tried and is not worth it:
`miniz.h` includes `miniz_zip.h` unconditionally, and `miniz_export.h` only exists once CMake has
generated it, so a deflate-only subset ends up as nine files instead of two - one of them not
upstream - and stops matching any released artifact.
