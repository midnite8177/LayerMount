#requires -Version 7.0

BeforeDiscovery {
  $script:DotnetMissing = $null -eq (Get-Command dotnet -ErrorAction SilentlyContinue)
}

BeforeAll {
  Import-Module (Join-Path $PSScriptRoot 'PagingReadRepro.psm1') -Force

  function New-Capture {
    param(
      [int]$ExitCode,
      [string]$StdOut,
      [string]$StdErr
    )
    [PSCustomObject]@{ ExitCode = $ExitCode; StdOut = $StdOut; StdErr = $StdErr; TimedOut = $false }
  }
}

Describe 'Get-PagingReadMatrix' {
  BeforeAll {
    $script:Matrix = @(Get-PagingReadMatrix)
  }

  It 'expands five origins by eight sizes into forty cases' {
    $script:Matrix.Count | Should -Be 40
  }

  It 'covers every size for every origin' {
    $origins = @($script:Matrix | Select-Object -ExpandProperty Origin -Unique)
    $origins.Count | Should -Be 5
    foreach ($origin in $origins) {
      $sizes = @($script:Matrix | Where-Object { $_.Origin -eq $origin } | Select-Object -ExpandProperty Size)
      $sizes | Should -Be @(0, 1, 4095, 4096, 4097, 65536, 65537, 1048576)
    }
  }

  It 'gives each case a file name that carries its origin and size' {
    $case = $script:Matrix | Where-Object { $_.Origin -eq 'lower' -and $_.Size -eq 4097 }
    $case.FileName | Should -Be 'lower-4097.bin'
  }

  It 'names no two cases the same' {
    $names = @($script:Matrix | Select-Object -ExpandProperty FileName -Unique)
    $names.Count | Should -Be 40
  }

  It 'stages a metacopy-shell case at 2 MiB above its size' {
    $case = $script:Matrix | Where-Object { $_.Origin -eq 'metacopy-shell' -and $_.Size -eq 4097 }
    $case.StagedSize | Should -Be 2101249
  }

  It 'stages every other origin at its size' {
    $others = @($script:Matrix | Where-Object { $_.Origin -ne 'metacopy-shell' })
    $others.Count | Should -Be 32
    foreach ($case in $others) {
      $case.StagedSize | Should -Be $case.Size
    }
  }
}

Describe 'Get-MetacopyShellStagingOffset' {
  It 'is the offset between a metacopy-shell case size and its staged size' {
    $case = Get-PagingReadMatrix | Where-Object { $_.Origin -eq 'metacopy-shell' -and $_.Size -eq 65537 }
    Get-MetacopyShellStagingOffset | Should -Be ($case.StagedSize - $case.Size)
  }
}

Describe 'Format-ShellStateFootnote' {
  It 'says every upper file was a sparse file of its staged size when all were' {
    $states = @(
      [PSCustomObject]@{ Size = 0; StagedSize = 2097152; Length = 2097152; IsSparse = $true },
      [PSCustomObject]@{ Size = 4097; StagedSize = 2101249; Length = 2101249; IsSparse = $true }
    )
    Format-ShellStateFootnote -States $states | Should -Be 'After staging, every metacopy-shell upper file was a sparse file of its staged size.'
  }

  It 'names each row whose upper file was not sparse, had another length, or was missing' {
    $states = @(
      [PSCustomObject]@{ Size = 0; StagedSize = 2097152; Length = 2097152; IsSparse = $true },
      [PSCustomObject]@{ Size = 1; StagedSize = 2097153; Length = 2097153; IsSparse = $false },
      [PSCustomObject]@{ Size = 4095; StagedSize = 2101247; Length = 4095; IsSparse = $true },
      [PSCustomObject]@{ Size = 4096; StagedSize = 2101248; Length = $null; IsSparse = $false }
    )
    Format-ShellStateFootnote -States $states | Should -Be 'After staging, these metacopy-shell upper files were not a sparse file of the staged size: size 1: length 2097153, sparse=False; size 4095: length 4095, sparse=True; size 4096: no upper file.'
  }

  It 'accepts an empty set' {
    Format-ShellStateFootnote -States @() | Should -Be 'After staging, every metacopy-shell upper file was a sparse file of its staged size.'
  }
}

Describe 'Get-PatternBytes' {
  It 'returns an empty array for length zero' {
    $bytes = Get-PatternBytes -Length 0
    $bytes.Count | Should -Be 0
  }

  It 'places a line feed at every 64th byte and printable ASCII elsewhere' {
    $bytes = Get-PatternBytes -Length 130
    $bytes[0] | Should -Be 0x21
    $bytes[1] | Should -Be 0x22
    $bytes[63] | Should -Be 0x0A
    $bytes[127] | Should -Be 0x0A
    $bytes[90] | Should -Be 0x21
    $bytes[129] | Should -Be (0x21 + (129 % 90))
  }

  It 'produces the same byte for an offset whatever the total length' {
    $short = Get-PatternBytes -Length 100
    $long = Get-PatternBytes -Length 5000
    $short[99] | Should -Be $long[99]
  }
}

Describe 'Get-ExpectedLineCount' {
  It 'counts one line per 64 bytes and one more for a partial last line' {
    Get-ExpectedLineCount -Length 0 | Should -Be 0
    Get-ExpectedLineCount -Length 1 | Should -Be 1
    Get-ExpectedLineCount -Length 4096 | Should -Be 64
    Get-ExpectedLineCount -Length 4097 | Should -Be 65
    Get-ExpectedLineCount -Length 1048576 | Should -Be 16384
  }
}

Describe 'ConvertTo-FindResult' {
  It 'passes when find.exe counted the number of lines the size predicts' {
    $result = ConvertTo-FindResult -Capture (New-Capture -ExitCode 0 -StdOut '---------- R:\LOWER-4097.BIN: 65' -StdErr '') -Size 4097
    $result.Outcome | Should -Be 'pass'
  }

  It 'fails when find.exe counted fewer lines than the size predicts' {
    $result = ConvertTo-FindResult -Capture (New-Capture -ExitCode 0 -StdOut '---------- R:\LOWER-4097.BIN: 64' -StdErr '') -Size 4097
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Be 'counted 64 lines, expected 65'
  }

  It 'passes when find.exe found no line in an empty file' {
    $result = ConvertTo-FindResult -Capture (New-Capture -ExitCode 1 -StdOut '---------- R:\LOWER-0.BIN: 0' -StdErr '') -Size 0
    $result.Outcome | Should -Be 'pass'
  }

  It 'fails when find.exe found no line in a file that has content' {
    $result = ConvertTo-FindResult -Capture (New-Capture -ExitCode 1 -StdOut '---------- R:\LOWER-4097.BIN: 0' -StdErr '') -Size 4097
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Match 'no lines counted'
  }

  It 'fails on an I/O error and keeps the message' {
    $result = ConvertTo-FindResult -Capture (New-Capture -ExitCode 2 -StdOut '' -StdErr 'Access denied - X:\lower-1.bin') -Size 1
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Match 'Access denied'
  }

  It 'fails as timed out when find.exe did not exit in time, whatever it wrote' {
    $capture = [PSCustomObject]@{ ExitCode = 0; StdOut = '---------- R:\LOWER-4097.BIN: 65'; StdErr = ''; TimedOut = $true }
    $result = ConvertTo-FindResult -Capture $capture -Size 4097
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Be 'timed out'
  }
}

Describe 'ConvertTo-MappedReadResult' {
  It 'passes on exit code zero' {
    $result = ConvertTo-MappedReadResult -Capture (New-Capture -ExitCode 0 -StdOut '' -StdErr '')
    $result.Outcome | Should -Be 'pass'
  }

  It 'fails and keeps the stderr line on a reported error' {
    $result = ConvertTo-MappedReadResult -Capture (New-Capture -ExitCode 1 -StdOut '' -StdErr 'IOException HRESULT=0x80070026: Reached the end of the file.')
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Be 'IOException HRESULT=0x80070026: Reached the end of the file.'
  }

  It 'reports the NTSTATUS in hex when the process died without a message' {
    $result = ConvertTo-MappedReadResult -Capture (New-Capture -ExitCode -1073741818 -StdOut '' -StdErr '')
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Match '0xC0000006'
  }

  It 'fails as timed out when the program did not exit in time, whatever its exit code' {
    $capture = [PSCustomObject]@{ ExitCode = 0; StdOut = ''; StdErr = ''; TimedOut = $true }
    $result = ConvertTo-MappedReadResult -Capture $capture
    $result.Outcome | Should -Be 'fail'
    $result.Detail | Should -Be 'timed out'
  }
}

Describe 'Format-ResultTable' {
  BeforeAll {
    $rows = @(
      [PSCustomObject]@{
        Origin     = 'lower'
        Size       = 4097
        Find       = New-ObserverResult -Outcome 'pass' -Detail ''
        MappedRead = New-ObserverResult -Outcome 'fail' -Detail 'exit 0xC0000006'
      }
    )
    $script:Table = Format-ResultTable -Rows $rows
  }

  It 'starts with a header row and a separator' {
    $script:Table[0] | Should -Be '| Origin | Size | find.exe | Mapped read | Detail |'
    $script:Table[1] | Should -Be '|---|---:|---|---|---|'
  }

  It 'renders one row per case with both outcomes and the detail' {
    $script:Table.Count | Should -Be 3
    $script:Table[2] | Should -Be '| lower | 4097 | pass | fail | exit 0xC0000006 |'
  }

  It 'escapes a pipe inside the detail so the row stays one row' {
    $rows = @(
      [PSCustomObject]@{
        Origin     = 'lower'
        Size       = 1
        Find       = New-ObserverResult -Outcome 'fail' -Detail 'a | b'
        MappedRead = New-ObserverResult -Outcome 'pass' -Detail ''
      }
    )
    $table = Format-ResultTable -Rows $rows
    $table[2] | Should -Be '| lower | 1 | fail | pass | find.exe: a \| b |'
  }
}

Describe 'Set-PackageReferenceVersion' {
  It 'rewrites the version of the named package and leaves the others alone' {
    $csproj = @'
<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Other.Package" Version="1.2.3" />
    <PackageReference Include="LayerMount" Version="0.1.2" />
  </ItemGroup>
</Project>
'@
    $edited = Set-PackageReferenceVersion -ProjectXml $csproj -PackageId 'LayerMount' -Version '1.1.0-repro20260101000000'
    $edited | Should -Match '<PackageReference Include="LayerMount" Version="1.1.0-repro20260101000000" />'
    $edited | Should -Match '<PackageReference Include="Other.Package" Version="1.2.3" />'
  }

  It 'does not touch a package whose id only starts with the named id' {
    $csproj = @'
<PackageReference Include="LayerMount.Native" Version="0.1.2" />
<PackageReference Include="LayerMount" Version="0.1.2" />
'@
    $edited = Set-PackageReferenceVersion -ProjectXml $csproj -PackageId 'LayerMount' -Version '9.9.9'
    $edited | Should -Match '<PackageReference Include="LayerMount.Native" Version="0.1.2" />'
    $edited | Should -Match '<PackageReference Include="LayerMount" Version="9.9.9" />'
  }

  It 'throws when the project has no reference to the package' {
    { Set-PackageReferenceVersion -ProjectXml '<Project />' -PackageId 'LayerMount' -Version '9.9.9' } |
      Should -Throw
  }
}

Describe 'Set-ProjectReferencePath' {
  It 'points the reference to the named project at the new path and leaves the others alone' {
    $csproj = @'
<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <ProjectReference Include="..\Shared\Shared.csproj" />
    <ProjectReference Include="..\..\..\LayerMount\src\LayerMount.NET\LayerMount.NET.csproj" />
  </ItemGroup>
</Project>
'@
    $edited = Set-ProjectReferencePath -ProjectXml $csproj -ProjectFileName 'LayerMount.NET.csproj' -Path 'C:\engine\src\LayerMount.NET\LayerMount.NET.csproj'
    $edited | Should -Match '<ProjectReference Include="C:\\engine\\src\\LayerMount\.NET\\LayerMount\.NET\.csproj" />'
    $edited | Should -Match '<ProjectReference Include="\.\.\\Shared\\Shared\.csproj" />'
    $edited | Should -Not -Match 'LayerMount\\src'
  }

  It 'matches an Include written with forward slashes' {
    $csproj = '<ProjectReference Include="../../engine/src/LayerMount.NET/LayerMount.NET.csproj" />'
    $edited = Set-ProjectReferencePath -ProjectXml $csproj -ProjectFileName 'LayerMount.NET.csproj' -Path 'D:\e\LayerMount.NET.csproj'
    $edited | Should -Be '<ProjectReference Include="D:\e\LayerMount.NET.csproj" />'
  }

  It 'matches an Include whose case differs from the named file name' {
    $csproj = '<ProjectReference Include="..\engine\src\layermount.net\layermount.net.csproj" />'
    $edited = Set-ProjectReferencePath -ProjectXml $csproj -ProjectFileName 'LayerMount.NET.csproj' -Path 'D:\e\LayerMount.NET.csproj'
    $edited | Should -Be '<ProjectReference Include="D:\e\LayerMount.NET.csproj" />'
  }

  It 'does not touch a reference whose file name only ends with the named file name' {
    $csproj = @'
<ProjectReference Include="..\MyLayerMount.NET.csproj" />
<ProjectReference Include="..\LayerMount.NET.csproj" />
'@
    $edited = Set-ProjectReferencePath -ProjectXml $csproj -ProjectFileName 'LayerMount.NET.csproj' -Path 'X:\LayerMount.NET.csproj'
    $edited | Should -Match '<ProjectReference Include="\.\.\\MyLayerMount\.NET\.csproj" />'
    $edited | Should -Match '<ProjectReference Include="X:\\LayerMount\.NET\.csproj" />'
  }

  It 'throws when the project has no reference to the named project' {
    $csproj = '<ProjectReference Include="..\LayerMount.TestShared\LayerMount.TestShared.csproj" />'
    { Set-ProjectReferencePath -ProjectXml $csproj -ProjectFileName 'LayerMount.NET.csproj' -Path 'X:\LayerMount.NET.csproj' } |
      Should -Throw
  }
}

Describe 'Wait-MountPoint' {
  It 'returns when the mount point is already reachable' {
    $root = Join-Path $TestDrive 'mounted'
    New-Item -ItemType Directory -Path $root | Out-Null
    { Wait-MountPoint -MountPoint $root -Timeout ([TimeSpan]::FromMilliseconds(300)) } | Should -Not -Throw
  }

  It 'reports the mount point as unreachable when it never appears' {
    $root = Join-Path $TestDrive 'never-mounted'
    { Wait-MountPoint -MountPoint $root -Timeout ([TimeSpan]::FromMilliseconds(300)) } |
      Should -Throw -ExpectedMessage "*'$root' is not reachable although the host adapter reported it mounted*"
  }

  It 'returns when the mount point appears during the wait' {
    $root = Join-Path $TestDrive 'appears-later'
    $jobStartupAllowance = [TimeSpan]::FromSeconds(20)
    $job = Start-Job -ScriptBlock {
      param($Path)
      Start-Sleep -Milliseconds 100
      New-Item -ItemType Directory -Path $Path | Out-Null
    } -ArgumentList $root
    try {
      { Wait-MountPoint -MountPoint $root -Timeout $jobStartupAllowance } | Should -Not -Throw
    }
    finally {
      $job | Wait-Job | Remove-Job
    }
  }
}

Describe 'Get-EngineReferenceKind' {
  It 'reports PackageReference for a project that references the package' {
    $csproj = '<PackageReference Include="LayerMount" Version="1.1.0" />'
    Get-EngineReferenceKind -ProjectXml $csproj -PackageId 'LayerMount' -ProjectFileName 'LayerMount.NET.csproj' | Should -Be 'PackageReference'
  }

  It 'reports ProjectReference for a project that references the engine project' {
    $csproj = '<ProjectReference Include="..\..\..\LayerMount\src\LayerMount.NET\LayerMount.NET.csproj" />'
    Get-EngineReferenceKind -ProjectXml $csproj -PackageId 'LayerMount' -ProjectFileName 'LayerMount.NET.csproj' | Should -Be 'ProjectReference'
  }

  It 'reports nothing for a project that references another engine project or package' {
    $csproj = @'
<PackageReference Include="LayerMount.Native" Version="1.1.0" />
<ProjectReference Include="..\..\..\LayerMount\src\LayerMount.TestShared\LayerMount.TestShared.csproj" />
'@
    Get-EngineReferenceKind -ProjectXml $csproj -PackageId 'LayerMount' -ProjectFileName 'LayerMount.NET.csproj' | Should -BeNullOrEmpty
  }
}

Describe 'The mapped-read program' -Skip:$script:DotnetMissing {
  BeforeAll {
    $project = Join-Path (Split-Path -Parent $PSScriptRoot) 'src/LayerMount.MappedRead'
    dotnet build $project -c Release --nologo | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "dotnet build of '$project' exited with code $LASTEXITCODE."
    }
    $script:MappedReadDll = Join-Path $project 'bin/Release/net8.0/LayerMount.MappedRead.dll'

    # A newer runtime than the program targets may be the only one installed.
    $script:SavedRollForward = $env:DOTNET_ROLL_FORWARD
    $env:DOTNET_ROLL_FORWARD = 'Major'

    $script:PatternFile = Join-Path $TestDrive 'pattern-4097.bin'
    [System.IO.File]::WriteAllBytes($script:PatternFile, (Get-PatternBytes -Length 4097))
  }

  AfterAll {
    $env:DOTNET_ROLL_FORWARD = $script:SavedRollForward
  }

  It 'exits 0 when every byte of the file matches the pattern' {
    dotnet $script:MappedReadDll $script:PatternFile 4097 | Out-Null
    $LASTEXITCODE | Should -Be 0
  }

  It 'exits 2 when one byte of the file differs from the pattern' {
    $bytes = [System.IO.File]::ReadAllBytes($script:PatternFile)
    $bytes[4096] = $bytes[4096] -bxor 0xFF
    [System.IO.File]::WriteAllBytes($script:PatternFile, $bytes)

    dotnet $script:MappedReadDll $script:PatternFile 4097 2>$null | Out-Null
    $LASTEXITCODE | Should -Be 2
  }
}
