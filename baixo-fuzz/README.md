# Baixo Fuzz

A light, warm doom fuzz for bass guitar, as an LV2 plugin, with the clean low end kept
separate from the fuzz.

🇧🇷 Em português: fuzz leve e quente estilo doom, com o grave original limpo por baixo
da distorção.

## How it works

```
in ─► LR4 crossover ─┬─► lows ──► delay (aligns with the fuzz path) ──► Grave limpo ─┐
                     └─► highs ─► mid push ─► 4x oversampled 2-stage asymmetric      │
                                  clipper ─► DC block ─► Médios ─► Tom ─► Nível fuzz ─┴─► Volume
```

- **Clean low end.** The fuzz only gets the signal above *Divisão*, so the sub lows
  never turn into mud, and the original lows come back underneath. Both paths are
  time-aligned: in the linear regime the sum loses at most 0.1 dB around the split,
  so there is no hole in the low end.
- **Warm, not fizzy.** Two soft-clipping stages with opposite bias give tube-like even
  harmonics. *Tom* tames the top end.
- **No digital fizz.** The clipper runs at 4x oversampling with linear-phase FIR
  filters. At the most extreme setting (Fuzz 10, loud 2.7 kHz sine) the worst alias is
  55 dB below the note.
- **Constant level.** Make-up gain measured on a real bass DI keeps the output RMS
  within 0.1 dB of the input across the whole Fuzz range.
- Latency: 16 samples (0.33 ms at 48 kHz), reported to the host.

## Controls

| Name in host | Meaning | Default |
|---|---|---|
| Fuzz | Saturation: low = warm overdrive, high = full fuzz | 4 |
| Tom | Fuzz high cut (lower = darker) | 2500 Hz |
| Médios | Mid boost/cut at 650 Hz on the fuzz | +3 dB |
| Nível fuzz | Fuzz level | 0 dB |
| Grave limpo | Clean low level (−40 = off) | 0 dB |
| Divisão | Split frequency: clean below, fuzz above | 120 Hz |
| Volume | Output level | 0 dB |

For a plain full-range fuzz, set *Divisão* to 40 Hz and *Grave limpo* to −40.

Put a noise gate (e.g. [Baixo Gate](../baixo-gate/)) before it: fuzz brings up the hiss.

## Build, test, install

```bash
make && make test     # alignment + aliasing tests
make install          # to ~/.lv2
./test_fuzz baixo-fuzz.lv2/baixo_fuzz.so in.f32 out.f32 [fuzz tone mids fuzz_lvl clean_lvl split volume]
```

The shared crossover and oversampler are in [`../common/bass_split.h`](../common/bass_split.h).

## License

ISC, see [LICENSE](../LICENSE).
