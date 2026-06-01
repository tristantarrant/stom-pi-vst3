Stom-Pi VST3 Bridge
---------------------

VST3-to-LV2 bridge tooling for Stom-Pi. Allows VST3 plugins to be loaded
by mod-host as if they were native LV2 plugins.

### Components

- **`vst3-wrapper.so`** — Shared LV2 wrapper binary. Symlinked into each
  generated `.lv2` bundle; uses `lv2_lib_descriptor()` to read per-bundle
  config and load the correct VST3 via Carla's backend.

- **`stom-pi-vst3-scan`** — CLI tool that introspects a `.vst3` bundle
  and generates `wrapper.json` (parameter metadata, audio I/O, class ID).
  Used by stom-pi-builder to produce LV2 wrapper bundles.

### Building

```bash
git submodule update --init --recursive
make
```

### Installing

```bash
sudo make install
# Installs:
#   /usr/lib/stom-pi/vst3-wrapper.so
#   /usr/bin/stom-pi-vst3-scan
```

### Dependencies

- Carla backend (built from ildaeil submodule)
- LV2 development headers (`lv2-dev`)
- pkg-config
