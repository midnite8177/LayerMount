#requires -Version 7.0
<#
.SYNOPSIS
  Write a sync manifest that describes this repository's tracked files
  and ignore rules.

.DESCRIPTION
  sync-vm-test-checkout.ps1 updates a Windows test checkout to match
  this repository, but the Windows test VM has no git install. Run
  this script first, on a machine that has git, to capture what git
  would tell sync-vm-test-checkout.ps1: the tracked files from
  `git ls-files`, and the ignore patterns from core.excludesFile,
  `.git/info/exclude`, and every `.gitignore` file in the repository.
  It writes that information to a sync manifest file. Copy the sync
  manifest to the VM and pass it to sync-vm-test-checkout.ps1, which
  reads it instead of calling git.

.PARAMETER SourcePath
  The git working tree to read tracked files and ignore rules from.

.PARAMETER ManifestPath
  Where to write the sync manifest, as JSON.

.EXAMPLE
  ./write-vm-sync-manifest.ps1 -SourcePath . -ManifestPath ./vm-sync-manifest.json

  Run on the Mac side of the Parallels shared folder before running
  sync-vm-test-checkout.ps1 on the VM.
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [string]$SourcePath,

  [Parameter(Mandatory)]
  [string]$ManifestPath
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'VmSync.psm1') -Force

function ConvertTo-IgnorePatternRecord {
  param(
    [string]$Line,
    [string]$Scope
  )

  if ($Line.Length -eq 0 -or $Line.StartsWith('#')) {
    return $null
  }

  $pattern = $Line
  $negate = $false
  if ($pattern.StartsWith('!')) {
    $negate = $true
    $pattern = $pattern.Substring(1)
  }

  $directoryOnly = $false
  if ($pattern.EndsWith('/')) {
    $directoryOnly = $true
    $pattern = $pattern.Substring(0, $pattern.Length - 1)
  }

  if ($pattern.Length -eq 0) {
    return $null
  }

  $anchored = $pattern.Contains('/')
  if ($pattern.StartsWith('/')) {
    $pattern = $pattern.Substring(1)
  }

  New-SyncIgnorePattern -Pattern $pattern -Scope $Scope -DirectoryOnly:$directoryOnly -Anchored:$anchored -Negate:$negate
}

function Get-IgnorePatternsFromFile {
  param(
    [string]$FilePath,
    [string]$Scope
  )

  if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
    return @()
  }

  @(
    Get-Content -LiteralPath $FilePath |
      ForEach-Object { ConvertTo-IgnorePatternRecord -Line $_ -Scope $Scope } |
      Where-Object { $null -ne $_ }
  )
}

function Get-IgnorePatternsForRepository {
  param(
    [string]$SourcePath
  )

  $ignorePatterns = [System.Collections.Generic.List[object]]::new()

  $excludesFileConfig = & git -C $SourcePath config --get core.excludesFile 2>$null
  if ($LASTEXITCODE -eq 0 -and $excludesFileConfig) {
    $excludesFilePath = $excludesFileConfig
    if ($excludesFilePath.StartsWith('~/')) {
      $excludesFilePath = Join-Path $HOME $excludesFilePath.Substring(2)
    }
    foreach ($record in (Get-IgnorePatternsFromFile -FilePath $excludesFilePath -Scope '')) {
      $ignorePatterns.Add($record)
    }
  }

  $infoExcludePath = Join-Path $SourcePath '.git/info/exclude'
  foreach ($record in (Get-IgnorePatternsFromFile -FilePath $infoExcludePath -Scope '')) {
    $ignorePatterns.Add($record)
  }

  $orderedGitignoreFiles = Get-ChildItem -LiteralPath $SourcePath -Filter '.gitignore' -File -Recurse -Force |
    ForEach-Object {
      $relativeDir = ([System.IO.Path]::GetRelativePath($SourcePath, $_.DirectoryName)) -replace '\\', '/'
      [PSCustomObject]@{
        File        = $_
        RelativeDir = $relativeDir
      }
    } |
    Where-Object { -not (Test-UnderGitDir -RelativePath $_.RelativeDir) } |
    ForEach-Object {
      $relativeDir = $_.RelativeDir
      if ($relativeDir -eq '.') {
        $relativeDir = ''
      }
      $depth = if ($relativeDir -eq '') { 0 } else { ($relativeDir -split '/').Count }
      [PSCustomObject]@{
        File  = $_.File
        Scope = $relativeDir
        Depth = $depth
      }
    } |
    Sort-Object -Property Depth -Stable

  foreach ($entry in $orderedGitignoreFiles) {
    foreach ($record in (Get-IgnorePatternsFromFile -FilePath $entry.File.FullName -Scope $entry.Scope)) {
      $ignorePatterns.Add($record)
    }
  }

  # PowerShell unrolls a collection written to the output stream, so an
  # empty list here would reach the caller as $null instead of as an
  # empty collection. The comma operator stops that unrolling.
  , $ignorePatterns
}

try {
  if (-not (Test-Path -LiteralPath $SourcePath -PathType Container)) {
    throw "SourcePath '$SourcePath' does not exist or is not accessible."
  }

  $gitError = $null
  $isWorkTree = & git -C $SourcePath rev-parse --is-inside-work-tree 2>&1 |
    Tee-Object -Variable gitError
  if ($LASTEXITCODE -ne 0 -or $isWorkTree -ne 'true') {
    throw "SourcePath '$SourcePath' is not a usable git working tree: $gitError"
  }

  $trackedFiles = @(& git -C $SourcePath ls-files)
  if ($LASTEXITCODE -ne 0) {
    throw "git ls-files failed for '$SourcePath'."
  }

  $ignorePatterns = Get-IgnorePatternsForRepository -SourcePath $SourcePath

  $manifest = [PSCustomObject]@{
    trackedFiles   = $trackedFiles
    ignorePatterns = $ignorePatterns
  }

  ($manifest | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $ManifestPath -Encoding utf8

  Write-Host "Wrote a sync manifest for $($trackedFiles.Count) tracked file(s) and $($ignorePatterns.Count) ignore pattern(s) to '$ManifestPath'."
}
catch {
  Write-Error $_
  exit 1
}
