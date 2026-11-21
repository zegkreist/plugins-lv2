# Baixo Gate

A noise gate and live hiss reducer for bass guitar, as an LV2 plugin.

*Baixo* is Portuguese for *bass*. 🇧🇷 [Leia em português](README.pt-BR.md).

Most noise gates either chatter around the threshold, chop the end of the note, or
leave the hiss on while you play. Baixo Gate works on all three problems, with 1 ms
of latency, so you can monitor live while you play through an amp sim.

| | Before | After |
|---|---|---|
| Sustained notes with pauses | [sustained-note-before.flac](examples/sustained-note-before.flac) | [sustained-note-after.flac](examples/sustained-note-after.flac) |
| Playing, then muting | [playing-before.flac](examples/playing-before.flac) | [playing-after.flac](examples/playing-after.flac) |

The examples are a raw DI recording (USB interface, input gain at 100%) and the same
recording processed with the default settings.

## What it does

**1. A gate that does not chatter or cut notes**

- The detector only listens to 40 Hz–1.2 kHz, where bass notes live. Hiss is mostly
  high frequency, so it cannot hold the gate open or make it flutter.
- Hysteresis and hold: the gate opens at *Threshold* and closes only after the level
  has stayed below *Threshold − Hysteresis* for *Hold* ms.
- It closes with a linear-in-dB release (sounds like a natural decay) down to *Range*,
  not a hard mute.
- 1 ms lookahead, so the attack of the note is never swallowed.

**2. Hiss reduction while you play (multiband)**

When the gate opens, the hiss would normally come back with the note. Bass notes have
little energy above a few kHz, so the hiss is easy to hear on top of them.

- The signal is split into 13 half-octave bands (Linkwitz-Riley 4 crossovers from
  300 Hz to 13.6 kHz, with allpass compensation). With no reduction the bands sum back
  flat: 0.00 dB from 40 Hz to 16 kHz.
- Below 300 Hz (the fundamental) is never touched.
- Each upper band gets a Wiener-style gain `g = sqrt(1 − strength · N / E)`, limited
  by *Reducer depth*. Bands where the note has energy pass untouched. Bands that only
  contain hiss are turned down.
- The noise level N of each band is **learned automatically** whenever the gate is
  fully closed (the pauses between your phrases), so it follows your gain setting.
- Band power uses unbiased averaging, and the upper bands close in 15 ms. After a
  string attack the hiss is gone within ~50 ms, instead of trailing the attack.

**3. Tail filter**

A low-pass whose cutoff follows the note level. On a loud note it is fully open. As
the note decays it slides down to *Tail cutoff* and removes the "ssss" at the end of
the note without gating it.

## Measured results

On a real DI recording (noise floor −59 dBFS RMS, broadband hiss, no mains hum):

- Playing: output equals input. The bands sum flat and the gate stays open.
- Pauses: −41 dB of noise. Pure silence for 25 s: the gate never opened.
- During sustained notes: hiss above 5 kHz reduced by 15–28 dB, with the band below
  300 Hz identical to the input.
- After each string attack: hiss above 5 kHz back to −90 dB within ~50 ms.
- CPU: 0.24 s to process 20 s of stereo audio on one core (about 1.2%).

## Controls

Control names are shown in Portuguese in the host. In order:

| Name in host | Meaning | Default |
|---|---|---|
| Threshold (abre) | Level that opens the gate | −45 dB |
| Histerese | Hysteresis: closes this far below the threshold | 13 dB |
| Attack | Opening ramp | 1 ms |
| Hold | Time below the close point before closing | 100 ms |
| Release | Closing ramp | 300 ms |
| Range (atenuacao fechado) | Attenuation when closed (−90 = full mute) | −45 dB |
| Filtro de chiado | Tail filter amount (0 = off) | 0.8 |
| Chiado: corte minimo | Tail filter lowest cutoff | 2500 Hz |
| Profundidade do redutor | Reducer depth: max cut per band (0 = reducer off) | −30 dB |
| Força do redutor | Reducer strength: higher removes more hiss, too high dulls the string | 4 |
| Nivel detectado | *Meter:* detector level | – |
| Reducao atual | *Meter:* gate gain (0 = open) | – |
| Redutor atuando agora | *Meter:* average band reduction | – |

**Calibration:** mute the strings and watch *Nivel detectado*. Set *Threshold −
Hysteresis* a little below that value, and *Threshold* about 10 dB above it.

**Where to put it:** first in the chain, before any drive or amp sim. Before the
drive, the hiss sits far below the note. After the drive, the hiss is amplified with
the note, and no gate can separate the two cleanly.

The initial per-band noise values come from the interface I measured. They are only
a starting point and are replaced after the first pause.

## Build and install

Requires a C compiler, `make`, `pkg-config` and the LV2 headers (`lv2-dev` on
Debian/Ubuntu).

```bash
make              # build baixo-gate.lv2/baixo_gate.so
make install      # copy the bundle to ~/.lv2 (PREFIX=... to change)
make uninstall
```

Then rescan plugins in your host (Carla, Ardour, Reaper, etc.). The plugin URI is
`urn:zegkreist:lv2:baixo-gate`.

## Testing

`test_gate` loads the plugin the same way a host does and runs audio through it:

```bash
make test                                              # synthetic notes + hiss
./test_gate baixo-gate.lv2/baixo_gate.so sweep         # band split flatness
./test_gate baixo-gate.lv2/baixo_gate.so in.f32 out.f32 [thr hyst depth strength]
```

`in.f32` is mono 48 kHz raw float32. The test prints gate open/close events and the
input/output level every 0.5 s.

## License

ISC, see [LICENSE](../LICENSE).
