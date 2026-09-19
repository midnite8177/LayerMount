#requires -Version 7.0

BeforeAll {
  $script:ScriptPath = Join-Path $PSScriptRoot 'write-vm-sync-manifest.ps1'

  $script:WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vm-sync-manifest-tests-$([guid]::NewGuid())"
  New-Item -ItemType Directory -Path $script:WorkRoot -Force | Out-Null

  # The writer folds core.excludesFile from the global and system git config
  # into the manifest. An empty global config and no system config keep a
  # developer's own excludes out of every manifest these tests read.
  $script:SavedGitConfigGlobal = $env:GIT_CONFIG_GLOBAL
  $script:SavedGitConfigNoSystem = $env:GIT_CONFIG_NOSYSTEM
  $emptyGitConfig = Join-Path $script:WorkRoot 'empty-gitconfig'
  Set-Content -LiteralPath $emptyGitConfig -Value ''
  $env:GIT_CONFIG_GLOBAL = $emptyGitConfig
  $env:GIT_CONFIG_NOSYSTEM = '1'

  function Write-ManifestFor {
    param(
      [Parameter(Mandatory)]
      [string]$SourcePath
    )

    $manifestPath = Join-Path $script:WorkRoot "$([guid]::NewGuid()).json"
    & $script:ScriptPath -SourcePath $SourcePath -ManifestPath $manifestPath
    Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  }
}

AfterAll {
  $env:GIT_CONFIG_GLOBAL = $script:SavedGitConfigGlobal
  $env:GIT_CONFIG_NOSYSTEM = $script:SavedGitConfigNoSystem
  Remove-Item -LiteralPath $script:WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Describe 'write-vm-sync-manifest.ps1' {

  BeforeAll {
    $script:FixtureRepo = Join-Path ([System.IO.Path]::GetTempPath()) "vm-sync-manifest-fixture-$([guid]::NewGuid())"
    New-Item -ItemType Directory -Path $script:FixtureRepo -Force | Out-Null

    & git -C $script:FixtureRepo init -q
    & git -C $script:FixtureRepo config user.name 'Test User'
    & git -C $script:FixtureRepo config user.email 'test@example.com'

    New-Item -ItemType Directory -Path (Join-Path $script:FixtureRepo 'sub') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $script:FixtureRepo 'a.txt') -Value 'A' -NoNewline
    Set-Content -LiteralPath (Join-Path $script:FixtureRepo 'sub/b.txt') -Value 'B' -NoNewline
    Set-Content -LiteralPath (Join-Path $script:FixtureRepo '.gitignore') -Value (
      @('*.log', '[Bb]in/', '/build') -join "`n"
    )

    & git -C $script:FixtureRepo add 'a.txt' 'sub/b.txt' '.gitignore'
    & git -C $script:FixtureRepo commit -q -m 'Initial commit'

    $script:Manifest = Write-ManifestFor -SourcePath $script:FixtureRepo
  }

  AfterAll {
    Remove-Item -LiteralPath $script:FixtureRepo -Recurse -Force -ErrorAction SilentlyContinue
  }

  It 'lists the tracked files' {
    $script:Manifest.trackedFiles | Should -Contain 'a.txt'
    $script:Manifest.trackedFiles | Should -Contain 'sub/b.txt'
    $script:Manifest.trackedFiles | Should -Contain '.gitignore'
    $script:Manifest.trackedFiles.Count | Should -Be 3
  }

  It 'records the extension glob pattern from .gitignore' {
    $pattern = $script:Manifest.ignorePatterns | Where-Object { $_.Pattern -eq '*.log' }
    $pattern | Should -Not -BeNullOrEmpty
    $pattern.Scope | Should -Be ''
    $pattern.DirectoryOnly | Should -BeFalse
    $pattern.Anchored | Should -BeFalse
    $pattern.Negate | Should -BeFalse
  }

  It 'records the bracket-class directory pattern from .gitignore, stripped of its trailing slash' {
    $pattern = $script:Manifest.ignorePatterns | Where-Object { $_.Pattern -eq '[Bb]in' }
    $pattern | Should -Not -BeNullOrEmpty
    $pattern.Scope | Should -Be ''
    $pattern.DirectoryOnly | Should -BeTrue
    $pattern.Anchored | Should -BeFalse
    $pattern.Negate | Should -BeFalse
  }

  It 'strips the leading slash from a rooted pattern like /build, keeping it anchored' {
    $pattern = $script:Manifest.ignorePatterns | Where-Object { $_.Pattern -eq 'build' }
    $pattern | Should -Not -BeNullOrEmpty
    $pattern.Anchored | Should -BeTrue
  }

  It 'preserves the .gitignore file''s line order in the pattern list' {
    $logIndex = [array]::IndexOf($script:Manifest.ignorePatterns.Pattern, '*.log')
    $binIndex = [array]::IndexOf($script:Manifest.ignorePatterns.Pattern, '[Bb]in')
    $logIndex | Should -BeLessThan $binIndex
  }
}
