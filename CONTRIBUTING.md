# Contributing to LayerMount (engine)

This repo ships the engine — the native `LayerMount.dll`, its managed wrapper
`LayerMount.NET`, the shared test helpers in `LayerMount.TestShared`, and the
native unit / ABI test suites. Host adapters live in their own consumer
repositories and reference this engine via `<ProjectReference>` paths;
their prerequisites (driver SDKs, runtime licenses), integration tests,
PowerShell modules, and smoke procedures live with them.

## Prerequisites

- **Visual Studio 2026** with the **Desktop development with C++** workload
  (installs MSBuild, the Windows SDK, and the CRT/STL used by the project).
- **.NET 8 SDK** (installed alongside VS 2026 by default; needed for the
  managed projects and `dotnet test`).

Host-adapter prerequisites (driver SDKs, runtime licenses) are isolated to
the consumer repositories and are not required to build this engine.

## Build

From a Developer PowerShell or Developer Command Prompt at the repo root:

```powershell
msbuild LayerMount.sln /p:Configuration=Release /p:Platform=x64
```

Debug builds work too (`/p:Configuration=Debug`). The output drops under
`x64\<Config>\`. No CMake, no vcpkg — NuGet package restore happens during
MSBuild automatically.

To build only one host (and skip the dependencies of the other), see the
host-specific fragment.

## Running the test suites

### Native unit tests (host-agnostic)

`LayerMount.Tests` covers the engine internals (cache, copy-up, path resolver,
whiteouts, metadata ADS, VHD, VSS, security, compression). Fast, no
elevation required.

```powershell
vstest.console.exe x64\Release\LayerMount.Tests.dll /Platform:x64
```

### Native ABI tests (host-agnostic)

`LayerMount.AbiTests` exercises the public C ABI surface of `LayerMount.dll`
(exports, diagnostics, capability gates, host mount points, error shapes).
No elevation required.

```powershell
vstest.console.exe x64\Release\LayerMount.AbiTests.dll /Platform:x64
```

### Managed wrapper unit tests

`LayerMount.NET.Tests` covers the managed `LayerMount.NET` wrapper. No
elevation required; tests that need an adapter binary (formerly
`EndToEndMountTests`) have moved to the host-adapter test suites.

```powershell
dotnet test src\LayerMount.NET.Tests -c Release
```

### Host-adapter test suites

Native integration tests, host-spawning E2E tests, the PowerShell Pester
suite, and manual smoke procedures all live with the adapter they target.
See the consuming adapter repositories' `CONTRIBUTING.md`.

## Commit / PR conventions

Use [Conventional Commits](https://www.conventionalcommits.org/) prefixes:

- `feat(<area>):` — new capability
- `fix(<area>):` — bug fix (no API change)
- `refactor(<area>):` — internal restructuring
- `test(<area>):` — test additions or fixes
- `docs:` — documentation only
- `chore:` — tooling / build / non-code

Area tags used in this repo: `vhd`, `vss`, `cli`, `host`, `powershell`,
`build`. Reference the task number in the body when relevant.

A breaking change (anything that bumps the major version of either the
managed surface or the native ABI) **must** include `!` after the type or
a `BREAKING CHANGE:` footer in the commit body — Versionize uses these
markers to decide the next version.

## Versioning and releases

LayerMount follows [Semantic Versioning](https://semver.org/). The
canonical version lives in `version.props` at the repo root; the managed
projects pick it up through `Directory.Build.props`, and the native
`LM_VER_*` macros are generated from it by an MSBuild target
(`src/LayerMount.dll/LayerMount.version.targets` → `public/LayerMountVersion.h`,
which is `.gitignored`).

`LM_ABI_VERSION` is independent of the SemVer string and ticks only on
binary-breaking changes to the public C exports in `LayerMount.def`.
Bump it by editing `LayerMountAbiVersion` in `version.props`.

### 0.x policy

While LayerMount is on `0.x`, **minor bumps may carry breaking changes**
(per the SemVer spec's pre-1.0 guidance). Breaks must be called out in
the commit footer (`BREAKING CHANGE:`) so they appear in `CHANGELOG.md`.
Cutting `1.0.0` requires a deliberate review of both the public C ABI
and the managed wrapper surface.

### Cutting a release

Releases are produced by `.github/workflows/release.yml`, triggered
manually via **Actions → release → Run workflow** with optional inputs:

- `release-as` — force a specific version (e.g. `0.2.0`); leave blank
  to let Versionize compute the next version from conventional commits.
- `dry-run` — build and pack, but skip git push and `nuget push`.

The workflow:

1. Restores `dotnet versionize` from `.config/dotnet-tools.json`.
2. Runs `versionize` against the version anchor `build/LayerMount.Version.csproj`.
3. Runs `build/sync-version.ps1` to propagate the bumped `<Version>`
   into `version.props` (which is what the rest of the build reads).
4. Builds the solution for both `x64` and `ARM64`.
5. Packs `LayerMount.Native` and `LayerMount` to `artifacts/`.
6. Commits the version bump + `CHANGELOG.md` update, tags `vX.Y.Z`,
   pushes commit + tag, and `nuget push`es both packages.
7. Creates a GitHub Release with the `.nupkg` / `.snupkg` attached.

Required repository secret: `NUGET_API_KEY` (a nuget.org API key scoped
to `LayerMount` and `LayerMount.Native`).

# 

## Vendored third-party components

`vendor/zstd` and `vendor/nlohmann` are checked in as source. Dependabot
cannot watch them. Bump them manually when a security or feature update
warrants it, and update `THIRD_PARTY_NOTICES.md` if the upstream license
text or version changes.
