# Baixo Delay

A psychedelic, airy delay for bass guitar, as an LV2 plugin. The repeats are modulated,
diffused and spread in stereo, and the bass low end stays dry.

🇧🇷 Em português: delay psicodélico e aéreo para baixo. As repetições ondulam, se
espalham numa nuvem e abrem no estéreo, e o grave fica seco por baixo.

## How it works

```
in ──────────────────────────────────────────────────────────────► dry ──────────┐
 └─► mono ─► LR4 high-pass (Divisão) ─► (+) ─► Tom ─► loop HP ─► soft sat ─► delay │
                                         ▲                                    │   │
                                         │       modulated read (Modulação) ◄─┘   │
                                         │                 │                       │
                                         │    allpass cloud (Difusão) ─► Nível eco ┴─► out
                                         └── Repetições × L/R cross-feed (Abertura) ◄┘
```

- **Dry low end.** Only what's above *Divisão* gets repeated, so the echoes float over
  a tight, dry bass instead of turning into mud. A 50 Hz note repeats more than 30 dB
  quieter than a 1 kHz one.
- **Tape wow.** Two LFOs swing the read point, with left and right in quadrature, so
  the repeats wobble in pitch and swirl across the stereo field.
- **Cloud.** *Difusão* crossfades the crisp echo into one smeared by four allpasses in
  the feedback loop. Every repeat smears a bit more, close to a reverb at 100. The echo
  still starts exactly on *Tempo*, with no pre-echo.
- **Analog-style decay.** Each repeat goes through *Tom* and a gentle high-pass again,
  so it comes back darker and thinner.
- **Safe self-oscillation.** Soft saturation in the loop: *Repetições* near 100 sustains
  and builds without blowing up.
- **Tape glide.** Turning *Tempo* while echoes are ringing bends their pitch instead of
  clicking.
- The dry signal passes bit-exact. Zero latency. About 0.4% of one CPU core in the
  worst case.

## Controls

| Name in host | Meaning | Default |
|---|---|---|
| Tempo | Time between repeats | 420 ms |
| Repetições | Feedback; near 100 = endless, self-oscillating | 50 % |
| Nível eco | Echo level (−40 = off); the dry signal is always full | −4 dB |
| Tom | High cut on every repeat (lower = darker) | 2800 Hz |
| Divisão | Below this the bass doesn't repeat | 180 Hz |
| Modulação | Wow/chorus depth on the repeats | 4 |
| Velocidade | Modulation speed | 0.5 Hz |
| Difusão | 0 = crisp echoes, high = cloud | 35 % |
| Abertura | 0 = echoes centered, 100 = ping-pong | 70 % |

Starting points:

- **Spacey:** Tempo 520, Repetições 65, Tom 2200, Modulação 6, Velocidade 0.35,
  Difusão 70, Abertura 100.
- **Crisp ping-pong:** Tempo 375, Repetições 45, Tom 4000, Modulação 1.5, Difusão 0,
  Abertura 100.
- **Wild:** Repetições 95+, then turn *Tempo* slowly while it rings.

Put it after the amp sim. *Abertura* only makes sense if the host feeds both outputs
to your ears (stereo); on a mono output the echoes still work, without the movement.

## Build, test, install

```bash
make && make test     # echo timing, feedback, dry low end, ping-pong, stability
make install          # to ~/.lv2
./test_delay baixo-delay.lv2/baixo_delay.so in.f32 out.f32 [time fb level tone split mod rate diff width]
```

The file mode reads mono 48 kHz float32 and writes stereo interleaved float32, with a
6 s tail. The low-end high-pass comes from [`../common/bass_split.h`](../common/bass_split.h).

## License

ISC, see [LICENSE](../LICENSE).
