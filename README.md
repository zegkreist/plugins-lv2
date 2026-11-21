# lv2-plugins

LV2 audio plugins, made for playing bass live on Linux through an amp sim (Carla,
JACK/PipeWire). Plain C, no dependencies beyond the LV2 headers.

| Plugin | What it does |
|---|---|
| [Baixo Gate](baixo-gate/) | Noise gate without chatter or chopped notes, plus a multiband hiss reducer that keeps working while you play. 1 ms latency. |

## Build everything

```bash
make            # build all plugins
make install    # install all to ~/.lv2
make test       # run each plugin's offline tests
```

Each plugin folder also builds on its own, and has its own README.

## License

ISC, see [LICENSE](LICENSE).
