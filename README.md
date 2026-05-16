# LayerMount for Windows — engine

[![CI](https://github.com/midnite8177/LayerMount/actions/workflows/ci.yml/badge.svg)](https://github.com/midnite8177/LayerMount/actions/workflows/ci.yml)
[![NuGet (LayerMount)](https://img.shields.io/nuget/v/LayerMount.svg?label=LayerMount)](https://www.nuget.org/packages/LayerMount)
[![NuGet (LayerMount.Native)](https://img.shields.io/nuget/v/LayerMount.Native.svg?label=LayerMount.Native)](https://www.nuget.org/packages/LayerMount.Native)
[![Downloads](https://img.shields.io/nuget/dt/LayerMount.svg)](https://www.nuget.org/packages/LayerMount)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**LayerMount** is a Windows engine for **Linux-style overlay filesystems** (union mounts) — a writable upper layer, prioritized read-only lowers, copy-up on first write, with optional VHD/VHDX and VSS snapshot sources and `.lmnt` layer-image packing.

**What it isn't:** LayerMount is **not a filesystem driver.** It enforces overlay *semantics* — copy-up, whiteouts, layer priority, image packing — over real paths in a writable upper directory. Presenting the resulting overlay as a mountable filesystem (at a drive letter or directory) is the job of a separate **host adapter** that targets a userspace-filesystem driver. Host adapters live in their own repositories and depend on this engine via NuGet.

This repo ships the **engine**: the native library (`LayerMount.dll`) and its managed wrapper (`LayerMount.NET`). The engine is host-agnostic and links only system + CRT libraries (a post-build check enforces this).

## Install

### .NET 8+ consumers (managed wrapper, recommended)

```powershell
dotnet add package LayerMount
```

`LayerMount` is AnyCPU and transitively pulls in `LayerMount.Native`, which carries the engine DLL for `win-x64` and `win-arm64`. Windows-only at runtime.

### Native C/C++ consumers

```powershell
dotnet add package LayerMount.Native
```

`LayerMount.h` lands under `build\native\include\` inside the package; the `.def` is alongside under `build\native\`. Either link against the import library yourself or `LoadLibrary` the DLL at runtime.

## Versioning

LayerMount follows [Semantic Versioning](https://semver.org/) and ships two version axes:

- `LM_VER_MAJOR.LM_VER_MINOR.LM_VER_PATCH` (in `LayerMount.h`, mirrored on the managed `LayerMount.GetVersion()`) — the SemVer string. Both NuGet packages (`LayerMount` and `LayerMount.Native`) always carry the same version and are bumped in lockstep.
- `LM_ABI_VERSION` — independent of the SemVer string. Only ticks on binary-breaking changes to the public C exports in `LayerMount.def`; bumping it requires consumers to re-link.

LayerMount is currently in the `0.x` series. While on `0.x`, **minor bumps may carry breaking changes** (per the SemVer spec's pre-1.0 guidance) — those are called out explicitly in `CHANGELOG.md`. The contract stabilizes at `1.0.0`.

---

## Concepts

An overlay is composed of three path roles, borrowed from Linux `overlayfs`:

- **Upper** — the writable layer. All new writes land here. Required.
- **Lower** — read-only layers, prioritized left-to-right (first lower wins for reads). Zero or more.
- **Work dir** — scratch space used for atomic copy-up. Defaults to `<upperParent>\.layermount-work` if not supplied.

When a file exists only in a lower layer, reads pass through. The first write promotes (copies up) the file into the upper layer and subsequent access reads/writes the upper copy. Lower layers are never modified.

Two additional layer sources can stand in for a directory path:

- **VHD / VHDX** — attached at mount, the volume GUID is used as a lower. Requires admin.
- **VSS snapshot** — a snapshot of a volume taken at mount time, used as a lower. Requires admin.

For the deeper engine architecture (path resolution, whiteouts, copy-up flavors, metadata, caching), see [docs/engine/ARCHITECTURE.md](docs/engine/ARCHITECTURE.md).

---

## Features

- **Overlay semantics** — upper / lower / work paths with copy-up on first write, file and directory whiteouts, opaque-directory markers.
- **Lazy copy-up** — metacopy promotes metadata immediately and completes data copy on first read or close, keeping mount-time fast.
- **VHD / VHDX layers** — attach a virtual disk at mount time and use its volume GUID as a high-priority lower.
- **VSS snapshot layers** — take a Volume Shadow Copy at mount time and use it as a read-only lower without holding open file handles.
- **`.lmnt` layer images** — pack a directory tree into a portable zstd-compressed image with SHA-256 footer; supports differential packs against a base image and multi-image manifests.
- **Capability-gated fallbacks** — opt out of ADS, reparse points, sparse files, multiple streams, or NTFS ACLs and the engine routes around the missing feature instead of erroring.
- **Reparse-point and ADS preservation** — both surface on copy-up.
- **ACL preservation** — DACL on every copy-up; SACL too when the process holds `SE_SECURITY_NAME`.
- **Per-process access tracking** — optional access log and JSON-rule-driven gating, keyed by `(pid, image path, creation time)`.
- **Diagnostic events** — single managed event stream for warnings, copy-ups, whiteouts, and access denials.
- **Host-agnostic** — links only system + CRT libraries (a post-build check enforces this); zero dependency on any userspace-filesystem driver.
- **AOT-compatible managed wrapper** — `[LibraryImport]`-based P/Invoke with trim analysis enabled; ships as AnyCPU with per-RID native via `LayerMount.Native`.

## Quick start

```csharp
using LayerMount;

// Two-layer overlay: writable upper + one read-only lower.
var config = new LayerMountConfig
{
    UpperPath   = @"C:\overlays\my-app\upper",
    WorkDirPath = @"C:\overlays\my-app\work",
    LowerPaths  = new[] { @"C:\overlays\my-app\base" },
};

using var mount = LayerMount.Create(config);

// Diagnostic events: warnings, copy-up, whiteouts, denials.
mount.Event += (_, e) => Console.WriteLine($"[{e.Kind}] {e.Path}");

// Path resolution: tells you which layer a relative path resolves to,
// and surfaces whiteouts / opaque-dir markers.
var resolved = mount.ResolvePath(@"bin\app.exe");
Console.WriteLine($"{resolved.FullPath}  (origin: {resolved.Origin})");

// Pack the upper layer into a portable, zstd-compressed image.
mount.Images.Pack(
    sourceDir:    config.UpperPath,
    outputPath:   @"C:\images\my-app-0.1.lmnt",
    description:  "first build");
```

For mount-driven I/O (`OpenFile` / `CreateFile` / `Read` / `Write`), the engine takes Win32-style access masks and create options because it's designed to be wired straight to a userspace-filesystem driver's I/O requests. The managed unit tests under `src/LayerMount.NET.Tests/` are the most practical reference for the I/O patterns.

## API surface

The managed wrapper lives in the `LayerMount` namespace; the native C ABI is `src/LayerMount.dll/public/LayerMount.h` + `LayerMount.def`. Top-level entry points:

| Type | Purpose |
|---|---|
| `LayerMount` | Top-level overlay handle. Static factories `Create(LayerMountConfig)` and `CreateTransient(workDir)`; instance methods for path/file ops, stats, volume info; `Event` for diagnostics. |
| `LayerMountConfig` | Construction parameters: `UpperPath`, `WorkDirPath`, `LowerPaths`, `Capabilities`, process-tracking knobs. |
| `LayerMountFile` | Handle returned by `OpenFile` / `CreateFile`. `Read` / `Write` accept managed `Span<byte>` and an originator PID. |
| `LayerMount.Vhd` | `Create` / `Open` VHD/VHDX layer files; `ListLayers` against a manifest directory. |
| `LayerMount.Vss` | `CreateSnapshot` / `DeleteSnapshot` / `ListSnapshots` / `Cleanup`. Admin required. |
| `LayerMount.Images` | `Pack`, `PackDifferential`, `Unpack`, `Validate`, `GetMetadata`; `CreateManifest` / `GetManifest` for multi-image bundles. |
| `LayerMount.ProcessTracker` | Per-process access log and rule-based gating (config-gated). |

The native ABI exports the same surface as C functions; consumers who want to bypass the managed wrapper can `LoadLibrary` `LayerMount.dll` and call directly against `LayerMount.h`.

---

## Building from source

```powershell
msbuild LayerMount.sln /p:Configuration=Release /p:Platform=x64
# Optional: cross-compile the native engine for arm64 as well
msbuild LayerMount.sln /p:Configuration=Release /p:Platform=ARM64
```

Outputs land in `<Platform>\<Configuration>\` (e.g. `x64\Release\` or `ARM64\Release\`). Debug builds work too (`/p:Configuration=Debug`). The native engine builds on both x64 and ARM64; test projects and the managed wrapper are x64 for `dotnet test`.

- `LayerMount.dll` (native engine)
- `LayerMount.NET.dll` (managed wrapper around the native engine)
- `LayerMount.Tests.dll`, `LayerMount.AbiTests.dll` (native test DLLs)
- `LayerMount.NET.Tests.dll`, `LayerMount.TestShared.dll` (managed wrapper tests + shared test helpers)

Tests:

```bash
# Native: VS Test Explorer / vstest.console.exe x64\<Config>\LayerMount.Tests.dll
dotnet test src/LayerMount.NET.Tests   # managed wrapper unit tests
```

Adapter-specific build prerequisites, exit codes, troubleshooting, and host-spawning E2E tests live with the adapter projects, not in this repo.

## Releases

Releases are cut by a manually-triggered GitHub Actions workflow (`release.yml`) that runs [Versionize](https://github.com/versionize/versionize) over the conventional commits since the previous tag, bumps `version.props` and `CHANGELOG.md`, builds + packs both NuGet packages (`LayerMount` and `LayerMount.Native`) for `win-x64` and `win-arm64`, pushes them to nuget.org, and creates a GitHub Release with the `.nupkg` / `.snupkg` files attached.

See `CONTRIBUTING.md` for the full release procedure.

## License

[MIT](LICENSE). Bundled third-party components are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
