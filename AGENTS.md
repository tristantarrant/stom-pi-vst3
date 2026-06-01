# AGENTS.md

## Project Overview

Stom-Pi VST3 Bridge — provides `vst3-wrapper.so` (LV2 wrapper) and
`stom-pi-vst3-scan` (VST3 introspection tool) for the Stom-Pi pedalboard
platform.

## Build

```bash
git submodule update --init --recursive
make              # Build everything
make carla        # Build Carla backend only
make wrapper      # Build vst3-wrapper.so only
make scanner      # Build stom-pi-vst3-scan only
make clean        # Clean all
```

## Architecture

The wrapper uses `lv2_lib_descriptor(bundle_path, features)` — the LV2 core
API that receives the bundle path at load time. This lets a single `.so` serve
different plugin URIs by reading a per-bundle `wrapper.json`.

Carla's backend handles all VST3 lifecycle (loading, parameter management,
audio processing, state serialization). The wrapper is thin glue between LV2
port semantics and Carla's host API.

## Key Files

- `src/wrapper.cpp` — LV2 wrapper plugin
- `src/scanner.cpp` — VST3 scanner CLI
- `ildaeil/` — Git submodule providing Carla backend + DPF

## Related Projects

- `../stom-pi` — Main Stom-Pi server
- `../stom-pi-builder` — Plugin build system (builds VST3 plugins, generates LV2 TTL + modgui)
- `../stom-pi-modhost` — mod-host + JACK packaging
