#requires -Version 7.0

BeforeAll {
  $script:ScriptPath = Join-Path $PSScriptRoot 'write-vm-sync-manifest.ps1'
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

    $script:ManifestPath = Join-Path $script:FixtureRepo 'manifest.json'
    & $script:ScriptPath -SourcePath $script:FixtureRepo -ManifestPath $script:ManifestPath

    $script:Manifest = Get-Content -LiteralPath $script:ManifestPath -Raw | ConvertFrom-Json
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
