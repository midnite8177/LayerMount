# Security policy

## Supported versions

LayerMount is currently in the `0.x` series. While on `0.x`, **only the most
recent minor release** receives security fixes. Once `1.0.0` ships, the two
most recent minors will be supported.

| Version | Supported          |
| ------- | ------------------ |
| latest `0.x` | yes           |
| older `0.x`  | no            |

## Reporting a vulnerability

Please report security issues **privately** via GitHub's Private Security
Advisories on this repository:

1. Go to the repository's **Security** tab.
2. Click **Report a vulnerability**.
3. Describe the issue, including reproduction steps if possible, the
   LayerMount version (`LM_VER_MAJOR.LM_VER_MINOR.LM_VER_PATCH` from
   `LayerMount.h` or `LayerMount.GetVersion()` from the managed wrapper),
   the Windows build, and whether the issue is reachable from the native
   ABI, the managed wrapper, or both.

**Please do not file a public GitHub issue for security problems.** Public
disclosure before a fix is available puts users at risk.

### Response timeline

- **Acknowledgement:** within 72 hours of submission.
- **Initial assessment:** within 7 days.
- **Fix targeting:** depends on severity; critical issues are prioritized.

We'll keep you updated through the advisory thread and credit you in the
release notes unless you ask otherwise.

## Scope

In scope:

- The native engine (`LayerMount.dll` and the static-lib flavor).
- The managed wrapper (`LayerMount.NET`).
- The public C ABI (`src/LayerMount.dll/public/LayerMount.h`,
  `LayerMount.def`) and the control-pipe protocol documented in `README.md`.

Out of scope (report to the corresponding maintainers instead):

- Host adapters that consume this engine — they live in their own
  repositories.
- Userspace filesystem drivers (WinFsp, ProjFS, CBFS Connect) that host
  adapters target.
- Vulnerabilities in the vendored third-party components themselves — file
  upstream with [zstd](https://github.com/facebook/zstd/security) or
  [nlohmann/json](https://github.com/nlohmann/json/security). If a vendored
  version we ship is affected, also notify us so we can pull in the fix.
