# Post-build symbol-hygiene guard for LayerMount.dll.
#
# Validates two invariants the engine must maintain:
#   1. Imports — the DLL links ONLY against system and CRT libraries on
#      the allowlist below. Anything else (filesystem drivers, host-
#      adapter SDKs, third-party native dependencies) is a FAIL: the
#      engine must remain host-agnostic so any adapter can consume it.
#   2. Exports — only C symbols, no C++ mangled names. The public C ABI
#      is exposed via the .def file; any leaked C++ symbol is a FAIL.

param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Dll)) {
    Write-Host "verify-layermount-dll: DLL not found at '$Dll'"
    exit 1
}

# Allowlist of permitted imported DLL names (case-insensitive, anchored).
# Add only system or CRT dependencies. Never add a host-adapter or
# filesystem-driver DLL.
$AllowedImports = @(
    '^kernel32\.dll$',
    '^ntdll\.dll$',
    '^advapi32\.dll$',
    '^user32\.dll$',
    '^ole32\.dll$',
    '^oleaut32\.dll$',
    '^rpcrt4\.dll$',
    '^bcrypt\.dll$',
    '^virtdisk\.dll$',
    '^vssapi\.dll$',
    '^vcruntime140d?\.dll$',
    '^vcruntime140_1d?\.dll$',
    '^msvcp140d?\.dll$',
    '^ucrtbased?\.dll$',
    '^api-ms-win-[a-z0-9_-]+\.dll$'
) -join '|'

$importsPath = Join-Path $OutDir 'imports.txt'
$exportsPath = Join-Path $OutDir 'exports.txt'

& dumpbin /nologo /imports $Dll > $importsPath
if ($LASTEXITCODE -ne 0) {
    Write-Host 'verify-layermount-dll: dumpbin /imports failed'
    exit 1
}

$imported = Select-String -Path $importsPath -Pattern '^\s+([A-Za-z0-9_.-]+\.dll)\s*$' `
    | ForEach-Object { $_.Matches.Groups[1].Value } `
    | Sort-Object -Unique

$bad = $imported | Where-Object { $_ -inotmatch $AllowedImports }
if ($bad) {
    Write-Host 'verify-layermount-dll: FAIL -- LayerMount.dll imports disallowed library/libraries:'
    $bad | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host 'The engine must remain host-agnostic. If a new import is a legitimate'
    Write-Host 'system or CRT dependency, add its name to the allowlist in'
    Write-Host 'verify-layermount-dll.ps1. Never allow a host-adapter or filesystem-driver'
    Write-Host 'DLL through this check.'
    exit 1
}

& dumpbin /nologo /exports $Dll > $exportsPath
if ($LASTEXITCODE -ne 0) {
    Write-Host 'verify-layermount-dll: dumpbin /exports failed'
    exit 1
}

# C++ mangled names start with `??` (for operators / ctors / dtors / template
# instantiations) after the standard ordinal/hint/RVA prefix dumpbin emits.
# Any hit is a FAIL.
$mangled = Select-String -Path $exportsPath -Pattern '\s\?\?' -List
if ($mangled) {
    Write-Host 'verify-layermount-dll: FAIL -- LayerMount.dll has C++ mangled exports'
    exit 1
}

Write-Host 'LayerMount.dll symbol hygiene OK'
exit 0
