#requires -Version 7.0

BeforeAll {
  Import-Module (Join-Path $PSScriptRoot 'VmSync.psm1') -Force

  $script:ScriptPath = Join-Path $PSScriptRoot 'sync-vm-test-checkout.ps1'

  $script:EmptySource = Join-Path ([System.IO.Path]::GetTempPath()) "sync-vm-empty-source-$([guid]::NewGuid())"
  $script:EmptyDest = Join-Path ([System.IO.Path]::GetTempPath()) "sync-vm-empty-dest-$([guid]::NewGuid())"
  New-Item -ItemType Directory -Path $script:EmptySource -Force | Out-Null
  New-Item -ItemType Directory -Path $script:EmptyDest -Force | Out-Null

  $script:EmptyManifestPath = Join-Path $script:EmptySource 'manifest.json'
  ([PSCustomObject]@{ trackedFiles = @(); ignorePatterns = @() } | ConvertTo-Json) |
    Set-Content -LiteralPath $script:EmptyManifestPath

  . $script:ScriptPath -SourcePath $script:EmptySource -DestPath $script:EmptyDest -ManifestPath $script:EmptyManifestPath
}

AfterAll {
  Remove-Item -LiteralPath $script:EmptySource -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $script:EmptyDest -Recurse -Force -ErrorAction SilentlyContinue
}

Describe 'Get-SyncPlan' {
  It 'copies every source file and removes dest files the source does not have' {
    $plan = Get-SyncPlan -SourceFiles @('a.txt', 'sub/b.txt') -DestFiles @('a.txt', 'stale.txt')

    $plan.ToCopy | Should -Be @('a.txt', 'sub/b.txt')
    $plan.ToRemove | Should -Be @('stale.txt')
  }

  It 'treats a case difference alone as the same file' {
    $plan = Get-SyncPlan -SourceFiles @('A.txt') -DestFiles @('a.txt')

    $plan.ToRemove | Should -BeNullOrEmpty
  }
}

Describe 'sync-vm-test-checkout.ps1, run against a fixture with no git' {

  BeforeAll {
    $script:SourceRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sync-vm-source-$([guid]::NewGuid())"
    $script:DestRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sync-vm-dest-$([guid]::NewGuid())"

    New-Item -ItemType Directory -Path $script:SourceRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $script:SourceRoot 'sub') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $script:SourceRoot 'a.txt') -Value 'A' -NoNewline
    Set-Content -LiteralPath (Join-Path $script:SourceRoot 'sub/b.txt') -Value 'B' -NoNewline

    New-Item -ItemType Directory -Path $script:DestRoot -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $script:DestRoot 'a.txt') -Value 'OLD' -NoNewline
    Set-Content -LiteralPath (Join-Path $script:DestRoot 'stale.txt') -Value 'stale' -NoNewline
    Set-Content -LiteralPath (Join-Path $script:DestRoot 'build.log') -Value 'log data' -NoNewline
    New-Item -ItemType Directory -Path (Join-Path $script:DestRoot 'sub/emptydirsoon') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $script:DestRoot 'sub/emptydirsoon/onlyfile.txt') -Value 'x' -NoNewline
    New-Item -ItemType Directory -Path (Join-Path $script:DestRoot 'cache/empty') -Force | Out-Null

    $script:ManifestPath = Join-Path $script:SourceRoot 'manifest.json'
    $manifest = [PSCustomObject]@{
      trackedFiles   = @('a.txt', 'sub/b.txt')
      ignorePatterns = @(
        New-SyncIgnorePattern -Pattern '*.log' -Scope '' -DirectoryOnly:$false -Anchored:$false -Negate:$false
        New-SyncIgnorePattern -Pattern 'cache' -Scope '' -DirectoryOnly:$true -Anchored:$false -Negate:$false
      )
    }
    ($manifest | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $script:ManifestPath

    . $script:ScriptPath -SourcePath $script:SourceRoot -DestPath $script:DestRoot -ManifestPath $script:ManifestPath
  }

  AfterAll {
    Remove-Item -LiteralPath $script:SourceRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $script:DestRoot -Recurse -Force -ErrorAction SilentlyContinue
  }

  It 'overwrites a tracked file that changed' {
    Get-Content -LiteralPath (Join-Path $script:DestRoot 'a.txt') -Raw | Should -Be 'A'
  }

  It 'copies a tracked file missing from the destination' {
    Get-Content -LiteralPath (Join-Path $script:DestRoot 'sub/b.txt') -Raw | Should -Be 'B'
  }

  It 'removes a destination file that is neither tracked nor ignored' {
    Test-Path -LiteralPath (Join-Path $script:DestRoot 'stale.txt') | Should -BeFalse
  }

  It 'leaves an ignored destination file untouched' {
    Test-Path -LiteralPath (Join-Path $script:DestRoot 'build.log') | Should -BeTrue
    Get-Content -LiteralPath (Join-Path $script:DestRoot 'build.log') -Raw | Should -Be 'log data'
  }

  It 'prunes a directory left empty after removing its only file' {
    Test-Path -LiteralPath (Join-Path $script:DestRoot 'sub/emptydirsoon') | Should -BeFalse
  }

  It 'leaves an empty directory under an ignored directory in place' {
    Test-Path -LiteralPath (Join-Path $script:DestRoot 'cache/empty') | Should -BeTrue
  }
}
