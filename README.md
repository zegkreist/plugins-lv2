# lv2-plugins

LV2 audio plugins, made for playing bass live on Linux through an amp sim (Carla,
JACK/PipeWire). Plain C, no dependencies beyond the LV2 headers.

| Plugin | What it does |
|---|---|
| [Baixo Gate](baixo-gate/) | Noise gate without chatter or chopped notes, plus a multiband hiss reducer that keeps working while you play. 1 ms latency. |
| [Baixo Fuzz](baixo-fuzz/) | Light, warm doom fuzz. Clean low end split from the fuzz, time-aligned; 4x oversampled. 0.33 ms latency. |
| [Baixo Delay](baixo-delay/) | Psychedelic, airy delay: tape-wobbled, diffused, ping-pong repeats over a dry low end. Zero latency. |

## Install (prebuilt)

Download the latest `.tar.gz` from
[Releases](https://github.com/zegkreist/plugins-lv2/releases) (`x86_64` = regular PC,
`arm64` = Raspberry Pi 4/5 and other ARM boards) and extract it into `~/.lv2`:

```bash
mkdir -p ~/.lv2
tar -xzf plugins-lv2-<version>-linux-x86_64.tar.gz -C ~/.lv2
```

Then rescan plugins in your host (Carla, Ardour, Reaper...). Binaries are built and
tested by GitHub Actions on every tagged version.

## Build everything

```bash
make            # build all plugins
make install    # install all to ~/.lv2
make test       # run each plugin's offline tests
```

Build dependencies: a C compiler, `make`, `pkg-config` and the LV2 headers (`lv2-dev`
on Debian/Ubuntu).

Each plugin folder also builds on its own, and has its own README.

Distortion plugins share [`common/bass_split.h`](common/bass_split.h): an LR4 crossover
that keeps the bass lows clean under the distortion, and a 4x linear-phase oversampler. Baixo Delay
uses its crossover to keep the lows out of the repeats.

## License

ISC, see [LICENSE](LICENSE).
