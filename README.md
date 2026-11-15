# BareDAW PluginHost

Optional native plugin hosting for BareDAW. This repository adapts CLAP and
VST3 plugins to the public BareDAW device API. It is not required by BareDAW,
WASM, or embedded builds.

Here, **BareDAW** means the engine in the `BareDAW-core` repository. The
`-core` suffix distinguishes that repository from integrations such as this
one; PluginHost extends BareDAW but is not part of the engine itself.

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DBAREDAW_PLUGIN_HOST_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CLAP is enabled by default. VST3 is opt-in with
`-DBAREDAW_ENABLE_VST3=ON`; the pinned SDK is fetched only in that build.

Faust AOT devices and Faust JIT remain owned by BareDAW. The helper in
`scripts/build-faust-device.sh` is here because it emits a dynamic plugin for
the PluginHost ABI. Example post-build Faust plugins live under
`examples/faust/post-build` and are covered by the PluginHost test suite when
the Faust compiler is available.

