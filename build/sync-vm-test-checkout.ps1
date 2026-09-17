#requires -Version 7.0
<#
.SYNOPSIS
  Update a Windows test checkout to match this repository's tracked files.

.DESCRIPTION
  The Parallels VM used to build and test the Windows-only native code
  keeps its own checkout of this repository. If no one updates that
  checkout, it drifts from the repository. Edited files become old,
  and files removed from the repository stay in the checkout. The VM
  has no git install, so this script does not call git.

  Run write-vm-sync-manifest.ps1 first, on a machine that has git (for
  example the Mac side of the Parallels shared folder), to write a
  sync manifest: the tracked files and ignore patterns this script
  needs. Then run this script on the VM, pointed at that sync
  manifest, to update the destination checkout.

  The script copies each file the sync manifest lists as tracked. It
  deletes each file in the destination that the sync manifest neither
  lists as tracked nor covers with an ignore pattern. It removes empty
  directories left after a deletion. The script does not change a
  file an ignore pattern covers, such as build output. It also does
  not touch the `.git` directory.

.PARAMETER SourcePath
  The folder to copy files from, such as a Parallels shared folder
  reaching the repository's working tree.

.PARAMETER DestPath
  The checkout to update.

.PARAMETER ManifestPath
  The sync manifest written by write-vm-sync-manifest.ps1.

.EXAMPLE
  ./sync-vm-test-checkout.ps1 -SourcePath '\\Mac\Home\Projects\LayerMount' -DestPath 'C:\LayerMountTest' -ManifestPath '\\Mac\Home\Projects\LayerMount\vm-sync-manifest.json'

  The standard command on the Parallels VM this project's sessions use.
#>

[CmdletBinding(SupportsShouldProcess)]
param(
  [Parameter(Mandatory)]
  [string]$SourcePath,

  [Parameter(Mandatory)]
  [string]$DestPath,

  [Parameter(Mandatory)]
  [string]$ManifestPath
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'VmSync.psm1') -Force

function ConvertTo-DestFullPath {
  param(
    [string]$DestPath,
    [string]$RelativePath
  )

  Join-Path $DestPath ($RelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
}

function Get-SyncPlan {
  param(
    [string[]]$SourceFiles,
    [string[]]$DestFiles
  )

  # Windows treats file names as case-insensitive; git-tracked paths do
  # not. Compare with a case-insensitive set so a case difference alone
  # does not mark a file as missing from the source.
  $sourceSet = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]$SourceFiles,
    [System.StringComparer]::OrdinalIgnoreCase)

  $toRemove = @($DestFiles | Where-Object { -not $sourceSet.Contains($_) })

  [PSCustomObject]@{
    ToCopy   = @($SourceFiles)
    ToRemove = $toRemove
  }
}

function Get-RelativePathsUnderRoot {
  param(
    [string]$Root,
    [array]$IgnorePatterns
  )

  $all = @(
    Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
      ForEach-Object {
        ([System.IO.Path]::GetRelativePath($Root, $_.FullName)) -replace '\\', '/'
      } |
      Where-Object { -not (Test-UnderGitDir -RelativePath $_) }
  )

  $kept = @($all | Where-Object { -not (Test-PathIgnoredBySyncManifest -RelativePath $_ -IgnorePatterns $IgnorePatterns) })

  # PowerShell unrolls an array written to the output stream, so an empty
  # array here would reach the caller as $null instead of as an empty
  # array. The comma operator stops that unrolling.
  , $kept
}

function Remove-EmptyDirectories {
  param(
    [string]$Root
  )

  $removedAny = $true
  while ($removedAny) {
    $removedAny = $false

    $candidates = Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force |
      Where-Object {
        $relative = ([System.IO.Path]::GetRelativePath($Root, $_.FullName)) -replace '\\', '/'
        -not (Test-UnderGitDir -RelativePath $relative)
      } |
      Sort-Object { $_.FullName.Length } -Descending

    foreach ($candidate in $candidates) {
      $isEmpty = -not (Get-ChildItem -LiteralPath $candidate.FullName -Force | Select-Object -First 1)
      if ($isEmpty) {
        Remove-Item -LiteralPath $candidate.FullName -Force
        $removedAny = $true
      }
    }
  }
}

function Copy-OneTrackedFile {
  param(
    [string]$SourcePath,
    [string]$DestPath,
    [string]$RelativePath
  )

  $sourceFull = Join-Path $SourcePath ($RelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
  $destFull = ConvertTo-DestFullPath -DestPath $DestPath -RelativePath $RelativePath

  if (-not (Test-Path -LiteralPath $sourceFull)) {
    Write-Warning "Skipped '$RelativePath': listed as tracked but missing from the source folder."
    return
  }

  $destDir = Split-Path -Parent $destFull
  if ($destDir -and -not (Test-Path -LiteralPath $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
  }
  Copy-Item -LiteralPath $sourceFull -Destination $destFull -Force
  Write-Host "Copied:  $RelativePath"
}

function Remove-OneUntrackedFile {
  param(
    [string]$DestPath,
    [string]$RelativePath
  )

  $destFull = ConvertTo-DestFullPath -DestPath $DestPath -RelativePath $RelativePath
  Remove-Item -LiteralPath $destFull -Force
  Write-Host "Removed: $RelativePath"
}

try {
  if (-not (Test-Path -LiteralPath $SourcePath -PathType Container)) {
    throw "SourcePath '$SourcePath' does not exist or is not accessible."
  }

  if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "ManifestPath '$ManifestPath' does not exist or is not accessible."
  }

  $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
  $sourceFiles = @($manifest.trackedFiles)
  $ignorePatterns = @($manifest.ignorePatterns)

  if (Test-Path -LiteralPath $DestPath) {
    $destFiles = Get-RelativePathsUnderRoot -Root $DestPath -IgnorePatterns $ignorePatterns
  }
  else {
    $destFiles = @()
    if ($PSCmdlet.ShouldProcess($DestPath, 'Create directory')) {
      New-Item -ItemType Directory -Path $DestPath -Force | Out-Null
    }
  }

  $plan = Get-SyncPlan -SourceFiles $sourceFiles -DestFiles $destFiles

  foreach ($relative in $plan.ToCopy) {
    if ($PSCmdlet.ShouldProcess($relative, 'Copy into destination checkout')) {
      Copy-OneTrackedFile -SourcePath $SourcePath -DestPath $DestPath -RelativePath $relative
    }
  }

  foreach ($relative in $plan.ToRemove) {
    if ($PSCmdlet.ShouldProcess($relative, 'Remove from destination checkout')) {
      Remove-OneUntrackedFile -DestPath $DestPath -RelativePath $relative
    }
  }

  if ($PSCmdlet.ShouldProcess($DestPath, 'Remove empty directories')) {
    Remove-EmptyDirectories -Root $DestPath
  }

  Write-Host ''
  Write-Host "$($plan.ToCopy.Count) file(s) matched to copy, $($plan.ToRemove.Count) file(s) matched to remove."
}
catch {
  Write-Error $_
  exit 1
}
