#requires -Version 7.0

BeforeAll {
  Import-Module (Join-Path $PSScriptRoot 'VmSync.psm1') -Force
}

Describe 'Test-PathIgnoredBySyncManifest' {

  Context 'bracket-class directory-only pattern, like [Bb]in/' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '[Bb]in' -Scope '' -DirectoryOnly:$true -Anchored:$false -Negate:$false
      )
    }

    It 'ignores a file under a matching directory at any depth' {
      Test-PathIgnoredBySyncManifest -RelativePath 'bin/Debug/App.dll' -IgnorePatterns $patterns | Should -BeTrue
      Test-PathIgnoredBySyncManifest -RelativePath 'src/Foo/Bin/x.txt' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'keeps a file whose name merely starts with the directory name' {
      Test-PathIgnoredBySyncManifest -RelativePath 'binary.txt' -IgnorePatterns $patterns | Should -BeFalse
    }
  }

  Context 'plain extension glob, like *.dll' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '*.dll' -Scope '' -DirectoryOnly:$false -Anchored:$false -Negate:$false
      )
    }

    It 'ignores a file with the matching extension' {
      Test-PathIgnoredBySyncManifest -RelativePath 'App.dll' -IgnorePatterns $patterns | Should -BeTrue
      Test-PathIgnoredBySyncManifest -RelativePath 'src/deep/App.dll' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'keeps a file with a different extension' {
      Test-PathIgnoredBySyncManifest -RelativePath 'App.exe' -IgnorePatterns $patterns | Should -BeFalse
    }
  }

  Context 'anchored negation pair followed by a re-ignore, mirroring src/LayerMount.dll/' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '*.dll' -Scope '' -DirectoryOnly:$false -Anchored:$false -Negate:$false
        New-SyncIgnorePattern -Pattern 'src/LayerMount.dll' -Scope '' -DirectoryOnly:$true -Anchored:$true -Negate:$true
        New-SyncIgnorePattern -Pattern 'src/LayerMount.dll/**' -Scope '' -DirectoryOnly:$false -Anchored:$true -Negate:$true
        New-SyncIgnorePattern -Pattern 'src/LayerMount.dll/x64' -Scope '' -DirectoryOnly:$true -Anchored:$true -Negate:$false
      )
    }

    It 'keeps a file the negation pair uncovers' {
      Test-PathIgnoredBySyncManifest -RelativePath 'src/LayerMount.dll/Native.dll' -IgnorePatterns $patterns | Should -BeFalse
    }

    It 'ignores a file under the directory the later pattern re-ignores' {
      Test-PathIgnoredBySyncManifest -RelativePath 'src/LayerMount.dll/x64/Native.dll' -IgnorePatterns $patterns | Should -BeTrue
    }
  }

  Context 'root-scope directory pattern covering a deep descendant, like .beads/' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '.beads' -Scope '' -DirectoryOnly:$true -Anchored:$false -Negate:$false
      )
    }

    It 'ignores a file several directories below the matched directory' {
      Test-PathIgnoredBySyncManifest -RelativePath '.beads/dolt/x' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'keeps a file outside the matched directory' {
      Test-PathIgnoredBySyncManifest -RelativePath 'src/file.txt' -IgnorePatterns $patterns | Should -BeFalse
    }
  }

  Context 'doublestar-prefixed pattern, like **/.claude/scheduled_tasks.lock' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '**/.claude/scheduled_tasks.lock' -Scope '' -DirectoryOnly:$false -Anchored:$true -Negate:$false
      )
    }

    It 'ignores the file at the repository root' {
      Test-PathIgnoredBySyncManifest -RelativePath '.claude/scheduled_tasks.lock' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'ignores the file several directories deep' {
      Test-PathIgnoredBySyncManifest -RelativePath 'a/b/.claude/scheduled_tasks.lock' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'keeps a differently named file in the same directory' {
      Test-PathIgnoredBySyncManifest -RelativePath '.claude/scheduled_tasks.json' -IgnorePatterns $patterns | Should -BeFalse
    }
  }

  Context 'pattern scope' {
    BeforeAll {
      $patterns = @(
        New-SyncIgnorePattern -Pattern '*.tmp' -Scope 'sub/dir' -DirectoryOnly:$false -Anchored:$false -Negate:$false
      )
    }

    It 'ignores a file inside the pattern''s scope' {
      Test-PathIgnoredBySyncManifest -RelativePath 'sub/dir/file.tmp' -IgnorePatterns $patterns | Should -BeTrue
    }

    It 'keeps a file with the same name outside the pattern''s scope' {
      Test-PathIgnoredBySyncManifest -RelativePath 'other/file.tmp' -IgnorePatterns $patterns | Should -BeFalse
    }
  }

  Context 'no patterns match' {
    It 'keeps the file' {
      Test-PathIgnoredBySyncManifest -RelativePath 'src/file.txt' -IgnorePatterns @() | Should -BeFalse
    }
  }
}
