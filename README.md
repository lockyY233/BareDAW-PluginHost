# BareDAW PluginHost

Optional native plugin hosting for BareDAW. This repository adapts CLAP and
VST3 plugins to the public BareDAW Core device API. It is not required by Core,
WASM, or embedded builds.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DBAREDAW_PLUGIN_HOST_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CLAP is enabled by default. VST3 is opt-in with
`-DBAREDAW_ENABLE_VST3=ON`; the pinned SDK is fetched only in that build.

Faust AOT devices and Faust JIT remain owned by BareDAW Core. The helper in
`scripts/build-faust-device.sh` is here because it emits a dynamic plugin for
the PluginHost ABI. Example post-build Faust plugins live under
`examples/faust/post-build` and are covered by the PluginHost test suite when
the Faust compiler is available.

