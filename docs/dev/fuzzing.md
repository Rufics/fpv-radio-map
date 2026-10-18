# Fuzz testing

The C++ code that reads input from outside the program is fuzzed: a fuzzer feeds it millions of
generated inputs and watches for crashes, memory errors and undefined behaviour. The targets are
[libFuzzer](https://llvm.org/docs/LibFuzzer.html) programs in `fuzz/`, built with
AddressSanitizer and UndefinedBehaviorSanitizer over the whole tree.

## What is fuzzed

| Target | Code | Where its input comes from |
|---|---|---|
| `package` | `PackageVerifier::verify` on raw bytes: gzip and tar parsing, path checks, limits, `META/signature` | Module uploads by an admin, registry files |
| `signed_package` | The same check past the signature: file list, hashes, manifest, referenced files | Packages from any trusted signer, including third-party keys |
| `manifest` | `ManifestParser::parse`: JSON schema and the rules it cannot express | `manifest.json` inside a package |
| `semver` | `Version` and `VersionRange` parsing and comparison | Manifest versions and dependency ranges |
| `crypto_text` | Base64 and key text (`fromBase64`, `PublicKey::fromText`, `SecretKey::fromText`) | Package signatures, keys pasted into the trust store |
| `mission_schema` | `SchemaRegistry::check` against the mission and library schemas | Every mission and library item a client sends |
| `iso_time` | `parseIsoUtc` | Times in requests and module host calls |
| `weather` | `WeatherService::at` and `horizon` over a forecast response | The Open-Meteo service |
| `link_budget` | rf-core link budget and Bullington diffraction, rf-ext clutter, over generated terrain | DEM and land cover data |

Besides the sanitizers, each target checks properties of its own, for example that a parsed
version prints back to itself or that a manifest the schema accepts never makes the parser throw.
The comment at the top of each `fuzz/targets/*_fuzzer.cpp` says which.

Pure maths with no outside input (geodesy, Fresnel integrals, units) is covered by the unit tests
instead, against published values.

## Running it locally

The fuzz build needs Clang with its runtime libraries. On Ubuntu 24.04, on top of the packages in
[Contributing](../../CONTRIBUTING.md):

```
sudo apt-get install -y clang-18 libclang-rt-18-dev
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++-18 -DRFMAP_BUILD_FUZZERS=ON -DRFMAP_BUILD_TESTS=OFF
cmake --build build-fuzz --target rfmap_fuzzers
```

Replay the corpus (every seed and every input that once found a bug):

```
ctest --test-dir build-fuzz -L fuzz
```

Fuzz every target for five minutes, as many at once as there are CPU cores:

```
FUZZ_SECONDS=300 python3 fuzz/run.py
```

`FUZZ_TARGETS="manifest semver"` runs only those targets and `FUZZ_MERGE=1` minimises each
corpus afterwards.

Or one target by hand:

```
build-fuzz/fuzz/manifest_fuzzer -dict=fuzz/dict/json.dict build-fuzz/fuzz-work/corpus/manifest fuzz/corpus/manifest
```

A crash writes the input to a `crash-*` file and prints the stack. Run the target on that file
alone to reproduce it. With a compiler other than Clang, `RFMAP_BUILD_FUZZERS=ON` builds the
targets as replay programs only: they run their checked-in corpus, `fuzz/corpus/<target>/`, under
the sanitizers but cannot fuzz. Put an input there to replay it.

## In CI

The `Fuzz C++` job of `.github/workflows/ci.yml` runs on every pull request and push to main once
the C++ build and tests passed, and fuzzes each target for a minute. `.github/workflows/fuzz.yml`
fuzzes ten minutes per target every night, or what the manual run asks for, and keeps the corpus
it grew in the Actions cache, so every later run starts from it. Both go through
`.github/actions/fuzz`: it builds only the fuzz binaries, and takes them from the cache when the
sources did not change. A crash fails the run, and the crashing input is attached to it as the
`fuzz-crashes` artifact.

## When the fuzzer finds a bug

1. Download the input and reproduce the crash with the target.
2. Fix the bug.
3. Add the input to `fuzz/corpus/<target>/` as `regression-<what it was>`, in the same pull
   request. The corpus replay then runs it on every fuzz run from now on.

## Adding a target

Write `fuzz/targets/<name>_fuzzer.cpp` with `LLVMFuzzerTestOneInput`, add a line to
`fuzz/CMakeLists.txt` and a few small, valid inputs in `fuzz/corpus/<name>/`. Fuzz what reads
outside input first. Catch only the exceptions the code documents; anything else is a finding.
`fuzz/support.hpp` has `rfmap::fuzz::check` for properties, `rfmap::fuzz::refused` for a caught
exception (a refusal must carry a message) and `ByteReader` to turn bytes into numbers.
