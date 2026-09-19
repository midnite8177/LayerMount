#requires -Version 7.0

BeforeDiscovery {
  $script:DotnetMissing = $null -eq (Get-Command dotnet -ErrorAction SilentlyContinue)
}

BeforeAll {
  $repoRoot = Split-Path -Parent $PSScriptRoot
  $script:PropsPath = Join-Path $repoRoot 'src/LayerMount.NET/LayerMount.NET.props'
  $script:EngineRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $script:PropsPath) '../..'))

  function Get-NativeDllPath {
    param(
      [Parameter(Mandatory)] [string]$ConsumerProject,
      [Parameter(Mandatory)] [AllowEmptyCollection()] [string[]]$ExtraArguments
    )
    $arguments = @('msbuild', $ConsumerProject, '-getProperty:_NativeLayerMountDllPath', '-nologo') + $ExtraArguments
    $output = & dotnet @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
      throw "dotnet msbuild exited with code ${LASTEXITCODE}: $output"
    }
    # stderr is merged into $output, so a warning line can precede the value; the value is always the last line.
    $printed = ($output | Select-Object -Last 1).ToString().Trim()
    # MSBuild does not normalize separators in a property value; the props builds the path with backslashes.
    $platformSeparators = $printed.Replace('\', [IO.Path]::DirectorySeparatorChar)
    [IO.Path]::GetFullPath($platformSeparators)
  }
}

Describe 'LayerMount.NET.props native library path' -Skip:$script:DotnetMissing {
  BeforeAll {
    $script:ConsumerProject = Join-Path $TestDrive 'Consumer.proj'
    @"
<Project>
  <PropertyGroup>
    <Configuration>Release</Configuration>
  </PropertyGroup>
  <Import Project="$script:PropsPath" />
</Project>
"@ | Set-Content -Path $script:ConsumerProject -Encoding utf8

    $script:SolutionDir = Join-Path $TestDrive 'host'
    $staleCopyDir = Join-Path $script:SolutionDir 'x64/Release'
    New-Item -ItemType Directory -Path $staleCopyDir -Force | Out-Null
    Set-Content -Path (Join-Path $staleCopyDir 'LayerMount.dll') -Value 'stale copy' -Encoding utf8
  }

  It 'reads the engine folder when the solution folder holds its own copy of the library' {
    $solutionDir = $script:SolutionDir + [IO.Path]::DirectorySeparatorChar
    $path = Get-NativeDllPath -ConsumerProject $script:ConsumerProject -ExtraArguments @("-p:SolutionDir=$solutionDir")
    $path | Should -Not -BeLike "$script:SolutionDir*"
    $path | Should -BeLike "$script:EngineRoot*"
  }

  It 'reads the engine folder when no solution folder is set' {
    $path = Get-NativeDllPath -ConsumerProject $script:ConsumerProject -ExtraArguments @()
    $path | Should -BeLike "$script:EngineRoot*"
  }
}
