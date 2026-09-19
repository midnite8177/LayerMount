<#
.SYNOPSIS
  Reproduce the paging read symptom over a mounted overlay and write a
  table of what find.exe and a mapped-read program observe.

.DESCRIPTION
  Runs on a Windows machine that has the engine build prerequisites, a
  checkout of a host adapter, and that adapter's runtime license in a
  file. The script:

    1. builds the engine and packs both packages into a local folder
       under a unique prerelease version;
    2. copies the host adapter checkout into a throwaway directory,
       points the copy's engine reference at this checkout, and builds
       the copy. For a PackageReference the script pins the version to
       the packed one and adds the local folder as a package source.
       For a ProjectReference the script rewrites the Include to the
       absolute path of the engine project in this checkout, and the
       adapter build compiles that project with the same version
       suffix;
    3. installs the filesystem host driver through the adapter;
    4. stages an upper, a lower and a work directory, mounts the overlay
       in the background, and creates one file per case in the matrix
       that Get-PagingReadMatrix in PagingReadRepro.psm1 defines;
    5. runs find.exe and the mapped-read program over every case;
    6. writes a Markdown table, one row per case, to stdout and to
       result-table.md in the workspace;
    7. shuts the mount down through its control pipe and deletes the
       throwaway adapter copy.

  Every file holds the same deterministic byte pattern, so the run
  detects a page that reads back wrong as well as a read that fails.

  The license file content goes into the named environment variable of
  the adapter process only. The script never prints it.

  The script never edits the adapter checkout. The rewritten reference
  lands in the copy.

.PARAMETER AdapterRepo
  The host adapter checkout to copy. Exactly one project file under it
  must hold either a PackageReference to the LayerMount package or a
  ProjectReference to the engine's LayerMount.NET project.

.PARAMETER AdapterExe
  The adapter executable's file name, or its path relative to the
  adapter copy's root. After the build, the script searches the copy
  for a bare file name.

.PARAMETER InstallArgs
  The arguments that make the adapter install the filesystem host
  driver and report the result as one JSON object on stdout.

.PARAMETER LicenseEnvVarName
  The environment variable the adapter reads its runtime license from.

.PARAMETER LicenseFilePath
  The file that holds the runtime license. The script reads it once,
  trims it, and passes it to the adapter process through the
  environment.

.PARAMETER WorkspaceRoot
  A local directory for everything the run produces: the packed
  packages, the throwaway adapter copy, the layer directories, and the
  table. The script creates it if it is missing.

.PARAMETER MountPoint
  Where the adapter mounts the overlay: a drive letter such as R: or a
  directory path that does not exist yet.

.EXAMPLE
  ./Invoke-PagingReadRepro.ps1 -AdapterRepo 'D:\adapter' -AdapterExe 'Adapter.exe' -InstallArgs 'install','--json' -LicenseEnvVarName 'ADAPTER_LICENSE' -LicenseFilePath 'D:\adapter.key' -WorkspaceRoot 'C:\paging-read-repro' -MountPoint 'R:'

  Builds everything, installs the driver, mounts at R:, runs the matrix
  and writes C:\paging-read-repro\result-table.md.
#>

#requires -Version 7.0

[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [string]$AdapterRepo,

  [Parameter(Mandatory)]
  [string]$AdapterExe,

  [Parameter(Mandatory)]
  [string[]]$InstallArgs,

  [Parameter(Mandatory)]
  [string]$LicenseEnvVarName,

  [Parameter(Mandatory)]
  [string]$LicenseFilePath,

  [Parameter(Mandatory)]
  [string]$WorkspaceRoot,

  [Parameter(Mandatory)]
  [string]$MountPoint
)

$ErrorActionPreference = 'Stop'
# Invoke-Checked inspects $LASTEXITCODE itself, and some tools write to stderr on success.
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'PagingReadRepro.psm1') -Force

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:EnginePackageId = 'LayerMount'
$script:EngineProjectFileName = 'LayerMount.NET.csproj'
$script:EngineProjectPath = Join-Path $script:RepoRoot 'src\LayerMount.NET' $script:EngineProjectFileName
$script:ObserverTimeout = [TimeSpan]::FromMinutes(2)

function Write-Stage {
  param([string]$Message)
  Write-Host ''
  Write-Host "==> $Message"
}

function Invoke-Checked {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$WorkingDirectory
  )

  Write-Host ("    {0} {1}" -f $FilePath, ($Arguments -join ' '))
  Push-Location -LiteralPath $WorkingDirectory
  try {
    & $FilePath @Arguments 2>&1 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
      throw "'$FilePath' exited with code $LASTEXITCODE."
    }
  }
  finally {
    Pop-Location
  }
}

function New-ProcessStartInfo {
  param(
    [string]$FilePath,
    [hashtable]$Environment
  )

  $psi = [System.Diagnostics.ProcessStartInfo]::new($FilePath)
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.CreateNoWindow = $true
  foreach ($name in $Environment.Keys) { $psi.Environment[$name] = $Environment[$name] }
  $psi
}

function Start-CapturedProcess {
  param(
    [System.Diagnostics.ProcessStartInfo]$StartInfo,
    [TimeSpan]$Timeout
  )

  $StartInfo.RedirectStandardError = $true
  $process = [System.Diagnostics.Process]::Start($StartInfo)
  $stdoutTask = $process.StandardOutput.ReadToEndAsync()
  $stderrTask = $process.StandardError.ReadToEndAsync()
  $timedOut = $false
  if (-not $process.WaitForExit([int]$Timeout.TotalMilliseconds)) {
    $timedOut = $true
    if (-not $process.HasExited) { $process.Kill($true) }
    $process.WaitForExit()
  }
  [void][System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask))

  [PSCustomObject]@{
    ExitCode = $process.ExitCode
    StdOut   = $stdoutTask.Result
    StdErr   = $stderrTask.Result
    TimedOut = $timedOut
  }
}

function Invoke-Captured {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [hashtable]$Environment,
    [TimeSpan]$Timeout
  )

  $psi = New-ProcessStartInfo -FilePath $FilePath -Environment $Environment
  foreach ($argument in $Arguments) { $psi.ArgumentList.Add($argument) }
  Start-CapturedProcess -StartInfo $psi -Timeout $Timeout
}

function Invoke-CapturedRaw {
  param(
    [string]$FilePath,
    [string]$RawArguments,
    [hashtable]$Environment,
    [TimeSpan]$Timeout
  )

  $psi = New-ProcessStartInfo -FilePath $FilePath -Environment $Environment
  $psi.Arguments = $RawArguments
  Start-CapturedProcess -StartInfo $psi -Timeout $Timeout
}

function Get-MsBuildPath {
  $onPath = Get-Command msbuild -ErrorAction SilentlyContinue
  if ($onPath) { return $onPath.Source }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'msbuild is not on PATH and vswhere.exe was not found. Run from a Developer PowerShell or install Visual Studio.'
  }
  $found = @(& $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')
  if ($found.Count -eq 0) {
    throw 'vswhere.exe found no MSBuild.exe.'
  }
  $found[0]
}

function Get-EngineVersion {
  <#
  .SYNOPSIS
    Read the release version from version.props and add a unique
    prerelease suffix so no package cache can serve a stale copy.
  #>
  $props = [xml](Get-Content -LiteralPath (Join-Path $script:RepoRoot 'version.props') -Raw)
  $group = $props.Project.PropertyGroup | Where-Object { $_.Label -eq 'LayerMountVersion' }
  $suffix = 'repro{0}' -f (Get-Date -Format 'yyyyMMddHHmmss')

  [PSCustomObject]@{
    Suffix = $suffix
    Full   = '{0}.{1}.{2}-{3}' -f $group.LayerMountVersionMajor, $group.LayerMountVersionMinor, $group.LayerMountVersionPatch, $suffix
  }
}

function Build-EngineAndPack {
  param(
    [string]$PackagesDir,
    [PSCustomObject]$EngineVersion
  )

  Write-Stage "Build the engine as $($EngineVersion.Full)"
  $msbuild = Get-MsBuildPath
  Invoke-Checked -FilePath $msbuild -WorkingDirectory $script:RepoRoot -Arguments @(
    'LayerMount.sln', '/p:Configuration=Release', '/p:Platform=x64',
    "/p:LayerMountVersionSuffix=$($EngineVersion.Suffix)", '/restore', '/m', '/v:minimal', '/nologo')

  Write-Stage "Pack both packages into $PackagesDir"
  if (Test-Path -LiteralPath $PackagesDir) { Remove-Item -LiteralPath $PackagesDir -Recurse -Force }
  New-Item -ItemType Directory -Path $PackagesDir -Force | Out-Null
  Invoke-Checked -FilePath 'dotnet' -WorkingDirectory $script:RepoRoot -Arguments @(
    'pack', 'src/LayerMount.Native.Package', '-c', 'Release', '--no-build', '-o', $PackagesDir,
    "-p:LayerMountVersionSuffix=$($EngineVersion.Suffix)", '-p:AllowPartialRidAssets=true', '--nologo')
  Invoke-Checked -FilePath 'dotnet' -WorkingDirectory $script:RepoRoot -Arguments @(
    'pack', 'src/LayerMount.NET', '-c', 'Release', '--no-build', '-o', $PackagesDir,
    "-p:LayerMountVersionSuffix=$($EngineVersion.Suffix)", '--nologo')
}

function Build-MappedReadProgram {
  Write-Stage 'Build the mapped-read program'
  $project = Join-Path $script:RepoRoot 'src\LayerMount.MappedRead'
  Invoke-Checked -FilePath 'dotnet' -WorkingDirectory $script:RepoRoot -Arguments @(
    'build', $project, '-c', 'Release', '--nologo')

  $exe = Join-Path $project 'bin\Release\net8.0\LayerMount.MappedRead.exe'
  if (-not (Test-Path -LiteralPath $exe)) {
    throw "The build did not produce the mapped-read program at '$exe'."
  }
  $exe
}

function Copy-FileToRelativePath {
  param(
    [string]$Root,
    [System.IO.FileInfo]$File,
    [string]$Destination
  )

  $relative = [System.IO.Path]::GetRelativePath($Root, $File.FullName)
  $target = Join-Path $Destination $relative
  $targetDir = Split-Path -Parent $target
  if (-not (Test-Path -LiteralPath $targetDir)) {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
  }
  Copy-Item -LiteralPath $File.FullName -Destination $target -Force
}

function Copy-AdapterCheckout {
  param(
    [string]$Source,
    [string]$Destination
  )

  $skipDirectories = @('.git', '.vs', '.idea', 'bin', 'obj', 'x64', 'arm64', 'artifacts', 'TestResults')

  if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
  New-Item -ItemType Directory -Path $Destination -Force | Out-Null

  $files = Get-ChildItem -LiteralPath $Source -File -Recurse -Force | Where-Object {
    $relative = [System.IO.Path]::GetRelativePath($Source, $_.DirectoryName)
    $segments = @($relative -split '[\\/]' | Where-Object { $_ -and $_ -ne '.' })
    -not ($segments | Where-Object { $skipDirectories -contains $_ })
  }

  $count = 0
  foreach ($file in $files) {
    Copy-FileToRelativePath -Root $Source -File $file -Destination $Destination
    $count++
  }
  Write-Host "    Copied $count file(s) into $Destination"
}

function Set-AdapterCopyEngineReference {
  <#
  .SYNOPSIS
    Point the adapter copy's engine reference at the engine this run
    built. Return the project file's path and the build properties
    the adapter build needs for that reference.
  #>
  param(
    [PSCustomObject]$WorkspacePaths,
    [PSCustomObject]$EngineVersion
  )

  $projects = @(Get-ChildItem -LiteralPath $WorkspacePaths.AdapterCopy -Filter '*.csproj' -File -Recurse | ForEach-Object {
      $kind = Get-EngineReferenceKind -ProjectXml (Get-Content -LiteralPath $_.FullName -Raw) -PackageId $script:EnginePackageId -ProjectFileName $script:EngineProjectFileName
      if ($kind) { [PSCustomObject]@{ Path = $_.FullName; Kind = $kind } }
    })

  if ($projects.Count -ne 1) {
    throw "Expected exactly one project under '$($WorkspacePaths.AdapterCopy)' with a PackageReference to '$($script:EnginePackageId)' or a ProjectReference to '$($script:EngineProjectFileName)', found $($projects.Count)."
  }

  $project = $projects[0].Path
  $original = Get-Content -LiteralPath $project -Raw

  if ($projects[0].Kind -eq 'ProjectReference') {
    $rewritten = Set-ProjectReferencePath -ProjectXml $original -ProjectFileName $script:EngineProjectFileName -Path $script:EngineProjectPath
    $message = "Pointed the ProjectReference to $($script:EngineProjectPath) in $project"
    # A project reference compiles the wrapper from source. Without the suffix its version lacks the run's stamp and Assert-AdapterUsesBuiltEngine throws.
    $buildProperties = @("-p:LayerMountVersionSuffix=$($EngineVersion.Suffix)")
  }
  else {
    $rewritten = Set-PackageReferenceVersion -ProjectXml $original -PackageId $script:EnginePackageId -Version $EngineVersion.Full
    $message = "Pinned $($script:EnginePackageId) to $($EngineVersion.Full) in $project"
    $buildProperties = @()

    $nugetConfig = @"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <add key="paging-read-repro-local" value="$($WorkspacePaths.PackagesDir)" />
  </packageSources>
</configuration>
"@
    Set-Content -LiteralPath (Join-Path $WorkspacePaths.AdapterCopy 'nuget.config') -Value $nugetConfig -Encoding utf8
  }

  Set-Content -LiteralPath $project -Value $rewritten -NoNewline -Encoding utf8
  Write-Host "    $message"

  [PSCustomObject]@{
    Path            = $project
    BuildProperties = $buildProperties
  }
}

function Find-AdapterExe {
  param(
    [string]$AdapterCopy,
    [string]$AdapterExe
  )

  if ($AdapterExe -match '[\\/]') {
    $candidate = Join-Path $AdapterCopy $AdapterExe
    if (-not (Test-Path -LiteralPath $candidate)) {
      throw "The build did not produce the adapter executable at '$candidate'."
    }
    return $candidate
  }

  $found = @(Get-ChildItem -LiteralPath $AdapterCopy -Filter $AdapterExe -File -Recurse |
      Where-Object { $_.FullName -notmatch '[\\/]obj[\\/]' })
  if ($found.Count -ne 1) {
    throw "Expected exactly one '$AdapterExe' under '$AdapterCopy' outside obj directories, found $($found.Count)."
  }
  $found[0].FullName
}

function Assert-AdapterUsesBuiltEngine {
  param(
    [string]$AdapterExePath,
    [string]$VersionString
  )

  $wrapper = Join-Path (Split-Path -Parent $AdapterExePath) 'LayerMount.NET.dll'
  if (-not (Test-Path -LiteralPath $wrapper)) {
    throw "The engine's managed wrapper is not next to the adapter at '$wrapper'."
  }
  $productVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($wrapper).ProductVersion
  if (-not $productVersion.StartsWith($VersionString, [System.StringComparison]::Ordinal)) {
    throw "The adapter build picked up engine version '$productVersion', not this run's '$VersionString'."
  }
  Write-Host "    Adapter runs engine $productVersion"
}

function Build-AdapterCopy {
  param(
    [string]$AdapterRepo,
    [string]$AdapterExe,
    [PSCustomObject]$WorkspacePaths,
    [PSCustomObject]$EngineVersion
  )

  Write-Stage "Copy the adapter checkout to $($WorkspacePaths.AdapterCopy)"
  Copy-AdapterCheckout -Source $AdapterRepo -Destination $WorkspacePaths.AdapterCopy

  $reference = Set-AdapterCopyEngineReference -WorkspacePaths $WorkspacePaths -EngineVersion $EngineVersion

  Write-Stage 'Build the adapter copy'
  Invoke-Checked -FilePath 'dotnet' -WorkingDirectory $WorkspacePaths.AdapterCopy -Arguments (
    @('build', $reference.Path, '-c', 'Release') + $reference.BuildProperties + @('--nologo'))

  $exe = Find-AdapterExe -AdapterCopy $WorkspacePaths.AdapterCopy -AdapterExe $AdapterExe
  Assert-AdapterUsesBuiltEngine -AdapterExePath $exe -VersionString $EngineVersion.Full
  $exe
}

function Read-LicenseIntoEnvironment {
  param(
    [string]$LicenseFilePath,
    [string]$LicenseEnvVarName
  )

  if (-not (Test-Path -LiteralPath $LicenseFilePath -PathType Leaf)) {
    throw "LicenseFilePath '$LicenseFilePath' does not exist or is not accessible."
  }
  $value = (Get-Content -LiteralPath $LicenseFilePath -Raw).Trim()
  if (-not $value) {
    throw "LicenseFilePath '$LicenseFilePath' is empty."
  }
  @{ $LicenseEnvVarName = $value }
}

function Get-LastJsonLine {
  param([string]$Text)

  $line = @($Text -split "`r?`n" | Where-Object { $_.TrimStart().StartsWith('{') } | Select-Object -Last 1)
  if ($line.Count -eq 0) {
    throw "No JSON object found in the output:`n$Text"
  }
  $line[0] | ConvertFrom-Json
}

function Install-Driver {
  param(
    [string]$AdapterExePath,
    [string[]]$InstallArgs,
    [hashtable]$Environment
  )

  Write-Stage 'Install the filesystem host driver'
  Write-Host ("    {0} {1}" -f $AdapterExePath, ($InstallArgs -join ' '))
  $result = Invoke-Captured -FilePath $AdapterExePath -Arguments $InstallArgs -Environment $Environment -Timeout ([TimeSpan]::FromMinutes(5))
  if ($result.ExitCode -ne 0) {
    throw "Driver install exited with code $($result.ExitCode).`n$($result.StdOut)`n$($result.StdErr)"
  }

  $json = Get-LastJsonLine -Text $result.StdOut
  $rebootRequired = [bool]$json.rebootRequired
  Write-Host "    Driver install status '$($json.status)', rebootRequired=$rebootRequired"
  $rebootRequired
}

function New-LayerDirectories {
  param([string]$LayersRoot)

  if (Test-Path -LiteralPath $LayersRoot) { Remove-Item -LiteralPath $LayersRoot -Recurse -Force }
  $layers = [PSCustomObject]@{
    Upper = Join-Path $LayersRoot 'upper'
    Work  = Join-Path $LayersRoot 'work'
    Lower = Join-Path $LayersRoot 'lower0'
  }
  foreach ($path in @($layers.Upper, $layers.Work, $layers.Lower)) {
    New-Item -ItemType Directory -Path $path -Force | Out-Null
  }
  $layers
}

function Write-PatternFile {
  param(
    [string]$Path,
    [long]$Length
  )
  [System.IO.File]::WriteAllBytes($Path, (Get-PatternBytes -Length $Length))
}

function Copy-LowerFileUpThroughMount {
  param(
    [string]$Path,
    [long]$Size
  )

  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
  try {
    if ($Size -gt 0) {
      $stream.Position = 0
      $stream.WriteByte((Get-PatternBytes -Length 1)[0])
    }
    else {
      $stream.SetLength(0)
    }
  }
  finally { $stream.Dispose() }
}

function New-MetacopyShellByAttributeOpen {
  param([string]$Path)

  # A write of the last write time is the one attribute-only open that
  # .NET offers. An open with data access fills the shell before it returns.
  [System.IO.File]::SetLastWriteTimeUtc($Path, [DateTime]::UtcNow)
}

# One entry per origin. PreMount runs against the layer directories
# before the mount and receives the layers and the case. ThroughMount
# runs against the mounted path and receives that path and the case.
$script:OriginSteps = @{
  'upper-through-mount' = @{
    PreMount     = $null
    ThroughMount = { param($Path, $Case) Write-PatternFile -Path $Path -Length $Case.Size }
  }
  'metacopy-shell'      = @{
    PreMount     = { param($Layers, $Case) Write-PatternFile -Path (Join-Path $Layers.Lower $Case.FileName) -Length $Case.StagedSize }
    ThroughMount = { param($Path, $Case) New-MetacopyShellByAttributeOpen -Path $Path }
  }
  'lower-copied-up'     = @{
    PreMount     = { param($Layers, $Case) Write-PatternFile -Path (Join-Path $Layers.Lower $Case.FileName) -Length $Case.Size }
    ThroughMount = { param($Path, $Case) Copy-LowerFileUpThroughMount -Path $Path -Size $Case.Size }
  }
  'upper-before-mount'  = @{
    PreMount     = { param($Layers, $Case) Write-PatternFile -Path (Join-Path $Layers.Upper $Case.FileName) -Length $Case.Size }
    ThroughMount = $null
  }
  'lower'               = @{
    PreMount     = { param($Layers, $Case) Write-PatternFile -Path (Join-Path $Layers.Lower $Case.FileName) -Length $Case.Size }
    ThroughMount = $null
  }
}

function Assert-EveryOriginHasSteps {
  param([array]$Matrix)

  $missing = @($Matrix | Select-Object -ExpandProperty Origin -Unique | Where-Object { -not $script:OriginSteps.ContainsKey($_) })
  if ($missing.Count -gt 0) {
    throw "The origin table has no entry for: $($missing -join ', ')."
  }
}

function Set-PreMountOrigins {
  param(
    [PSCustomObject]$Layers,
    [array]$Matrix
  )

  Write-Stage 'Stage the files that exist before the mount'
  foreach ($case in $Matrix) {
    $step = $script:OriginSteps[$case.Origin].PreMount
    if ($null -ne $step) { & $step $Layers $case }
  }
}

function Start-Mount {
  param(
    [string]$AdapterExePath,
    [hashtable]$Environment,
    [PSCustomObject]$Layers,
    [string]$MountPoint
  )

  Write-Stage "Mount the overlay at $MountPoint"
  $psi = New-ProcessStartInfo -FilePath $AdapterExePath -Environment $Environment
  foreach ($argument in @('--json', 'mount', '--handshake-child',
      '-m', $MountPoint, '-u', $Layers.Upper, '-l', $Layers.Lower, '-w', $Layers.Work)) {
    $psi.ArgumentList.Add($argument)
  }
  $psi.RedirectStandardInput = $true
  $psi.WorkingDirectory = Split-Path -Parent $AdapterExePath

  $process = [System.Diagnostics.Process]::Start($psi)
  $readLine = $process.StandardOutput.ReadLineAsync()
  if (-not $readLine.Wait(60000)) {
    if (-not $process.HasExited) { $process.Kill($true) }
    throw 'The adapter did not emit a mount handshake within 60 seconds.'
  }
  $line = $readLine.Result
  if (-not $line) {
    $process.WaitForExit()
    throw "The adapter exited with code $($process.ExitCode) before emitting a mount handshake."
  }

  $handshake = $line | ConvertFrom-Json
  if ($handshake.status -ne 'mounted') {
    $process.WaitForExit(10000) | Out-Null
    throw "Mount failed: $line"
  }
  Write-Host "    Mounted. Host '$($handshake.host)', pid $($handshake.pid), control pipe $($handshake.controlPipe)"

  Wait-MountPoint -MountPoint $MountPoint -Timeout ([TimeSpan]::FromSeconds(15))

  [PSCustomObject]@{
    Process   = $process
    Handshake = $handshake
    # The task keeps reading the adapter's stdout, because a full pipe buffer blocks the adapter and the mount hangs.
    Drain     = $process.StandardOutput.ReadToEndAsync()
  }
}

function Stop-Mount {
  param([PSCustomObject]$Mount)

  Write-Stage 'Shut the mount down through its control pipe'
  $process = $Mount.Process
  try {
    $pipeName = $Mount.Handshake.controlPipe -replace '^\\\\[.?]\\pipe\\', ''
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
    try {
      $pipe.Connect(5000)
      $writer = [System.IO.StreamWriter]::new($pipe)
      $writer.AutoFlush = $true
      $writer.WriteLine('{"cmd":"shutdown"}')
      $reader = [System.IO.StreamReader]::new($pipe)
      $ack = $reader.ReadLine()
      if ([string]::IsNullOrEmpty($ack)) {
        Write-Host '    The control pipe closed without an acknowledgement. The script waits for the adapter to exit.'
      }
      else {
        Write-Host "    Shutdown acknowledged: $ack"
      }
    }
    finally {
      $pipe.Dispose()
    }
  }
  catch {
    Write-Warning "Control pipe shutdown failed: $_"
  }

  if (-not $process.WaitForExit(30000)) {
    Write-Warning 'The adapter did not exit within 30 seconds of the shutdown request. The script kills it.'
    if (-not $process.HasExited) { $process.Kill($true) }
    $process.WaitForExit(5000) | Out-Null
  }
  Write-Host "    Adapter exited with code $($process.ExitCode)"
}

function Set-ThroughMountOrigins {
  param(
    [array]$Matrix,
    [string]$MountPoint
  )

  Write-Stage 'Create the origins that go through the mount'
  $stagingErrors = @{}
  foreach ($case in $Matrix) {
    $path = Join-Path $MountPoint $case.FileName
    $step = $script:OriginSteps[$case.Origin].ThroughMount
    if ($null -eq $step) { continue }
    try {
      & $step $path $case
    }
    catch {
      $message = $_.Exception.GetBaseException().Message
      $stagingErrors[$case.FileName] = $message
      Write-Host ('    {0,-20} {1,8}  not staged: {2}' -f $case.Origin, $case.Size, $message)
    }
  }
  $stagingErrors
}

function Get-MetacopyShellStates {
  param(
    [array]$Matrix,
    [PSCustomObject]$Layers,
    [hashtable]$StagingErrors
  )

  Write-Stage 'Read each metacopy-shell upper file before any observer opens it'
  foreach ($case in $Matrix) {
    if ($case.Origin -ne 'metacopy-shell' -or $StagingErrors.ContainsKey($case.FileName)) { continue }
    $upperFile = [System.IO.FileInfo]::new((Join-Path $Layers.Upper $case.FileName))
    $length = if ($upperFile.Exists) { $upperFile.Length } else { $null }
    $isSparse = $upperFile.Exists -and (($upperFile.Attributes -band [System.IO.FileAttributes]::SparseFile) -ne 0)
    Write-Host ('    {0,-20} {1,8}  upper length={2} sparse={3}' -f $case.Origin, $case.Size, $length, $isSparse)
    [PSCustomObject]@{
      Size       = $case.Size
      StagedSize = $case.StagedSize
      Length     = $length
      IsSparse   = $isSparse
    }
  }
}

function Invoke-Observer {
  <#
  .SYNOPSIS
    Run one observer over one file and classify what it reported.

  .PARAMETER Start
    Starts the observer process. It receives the environment and the
    timeout every observer runs with and returns the capture.

  .PARAMETER Classify
    Receives the capture and returns the result object.
  #>
  param(
    [scriptblock]$Start,
    [scriptblock]$Classify
  )

  $capture = & $Start -Environment @{} -Timeout $script:ObserverTimeout
  & $Classify $capture
}

function Invoke-Observers {
  param(
    [array]$Matrix,
    [string]$MountPoint,
    [string]$MappedReadExePath,
    [hashtable]$StagingErrors
  )

  Write-Stage 'Run find.exe and the mapped-read program over every case'
  $findExe = Join-Path $env:SystemRoot 'System32\find.exe'

  foreach ($case in $Matrix) {
    $path = Join-Path $MountPoint $case.FileName

    if ($StagingErrors.ContainsKey($case.FileName)) {
      Write-Host ('    {0,-20} {1,8}  not staged' -f $case.Origin, $case.Size)
      [PSCustomObject]@{
        Origin     = $case.Origin
        Size       = $case.Size
        Find       = New-ObserverResult -Outcome 'fail' -Detail ('not staged: ' + $StagingErrors[$case.FileName])
        MappedRead = New-ObserverResult -Outcome 'fail' -Detail ''
      }
      continue
    }

    # find.exe parses its own command line and needs the search string
    # in double quotes, so the arguments go as one raw string.
    $findArguments = '/c /v "~" "{0}"' -f $path
    $findResult = Invoke-Observer `
      -Start { param($Environment, $Timeout) Invoke-CapturedRaw -FilePath $findExe -RawArguments $findArguments -Environment $Environment -Timeout $Timeout } `
      -Classify { param($Capture) ConvertTo-FindResult -Capture $Capture -Size $case.StagedSize }

    $mappedResult = Invoke-Observer `
      -Start { param($Environment, $Timeout) Invoke-Captured -FilePath $MappedReadExePath -Arguments @($path, [string]$case.StagedSize) -Environment $Environment -Timeout $Timeout } `
      -Classify { param($Capture) ConvertTo-MappedReadResult -Capture $Capture }

    Write-Host ('    {0,-20} {1,8}  read at {2,8}  find.exe={3,-4}  mapped={4}' -f $case.Origin, $case.Size, $case.StagedSize, $findResult.Outcome, $mappedResult.Outcome)

    [PSCustomObject]@{
      Origin     = $case.Origin
      Size       = $case.Size
      Find       = $findResult
      MappedRead = $mappedResult
    }
  }
}

function Get-TableFootnotes {
  param(
    [string]$EngineVersion,
    [PSCustomObject]$Handshake,
    [string]$MountPoint,
    [bool]$RebootRequired,
    [array]$ShellStates
  )

  $offsetMiB = (Get-MetacopyShellStagingOffset) / 1MB
  @(
    '',
    "Engine $EngineVersion, mounted by host '$($Handshake.host)' at $MountPoint. Every file holds the pattern byte(i) = LF when i % 64 == 63, else 0x21 + (i % 90).",
    "metacopy-shell rows: the script staged the lower file at $offsetMiB MiB plus the row's size. It set the file's last write time through the mount, which opens the file for attributes only and leaves a sparse shell of that size in the upper. The script wrote no data to it. The observers read the file at $offsetMiB MiB plus the row's size. A row marked not staged names the exception that step threw.",
    (Format-ShellStateFootnote -States $ShellStates),
    'lower-copied-up rows: the script opened the lower file for read and write through the mount and rewrote its first byte with the same pattern value. For the 0-byte row it set the size to 0 instead.',
    'The same mount session staged and observed every file. find.exe ran as find.exe /c /v "~" <file> and passes when its line count is the one the size predicts, one line per 64 bytes. The mapped-read program maps a read-only view and compares every byte with the pattern.',
    $(if ($RebootRequired) { 'The driver install reported that a reboot is required. The machine did not reboot before this run.' } else { $null })
  ) | Where-Object { $null -ne $_ }
}

try {
  $matrix = @(Get-PagingReadMatrix)
  Assert-EveryOriginHasSteps -Matrix $matrix

  if (-not (Test-Path -LiteralPath $AdapterRepo -PathType Container)) {
    throw "AdapterRepo '$AdapterRepo' does not exist or is not accessible."
  }
  New-Item -ItemType Directory -Path $WorkspaceRoot -Force | Out-Null
  $workspace = (Resolve-Path -LiteralPath $WorkspaceRoot).Path

  $workspacePaths = [PSCustomObject]@{
    PackagesDir = Join-Path $workspace 'packages'
    AdapterCopy = Join-Path $workspace 'adapter'
    TableFile   = Join-Path $workspace 'result-table.md'
  }

  $licenseEnvironment = Read-LicenseIntoEnvironment -LicenseFilePath $LicenseFilePath -LicenseEnvVarName $LicenseEnvVarName

  $engineVersion = Get-EngineVersion
  Build-EngineAndPack -PackagesDir $workspacePaths.PackagesDir -EngineVersion $engineVersion
  $mappedReadExePath = Build-MappedReadProgram
  $adapterExePath = Build-AdapterCopy -AdapterRepo $AdapterRepo -AdapterExe $AdapterExe -WorkspacePaths $workspacePaths -EngineVersion $engineVersion

  $rebootRequired = Install-Driver -AdapterExePath $adapterExePath -InstallArgs $InstallArgs -Environment $licenseEnvironment

  $layers = New-LayerDirectories -LayersRoot (Join-Path $workspace 'layers')
  Set-PreMountOrigins -Layers $layers -Matrix $matrix

  $mount = $null
  try {
    $mount = Start-Mount -AdapterExePath $adapterExePath -Environment $licenseEnvironment -Layers $layers -MountPoint $MountPoint
  }
  catch {
    if ($rebootRequired) {
      throw "The mount failed after a driver install that requires a reboot. Reboot the machine and run the script again. $_"
    }
    throw
  }

  $rows = @()
  try {
    $stagingErrors = Set-ThroughMountOrigins -Matrix $matrix -MountPoint $MountPoint
    $shellStates = @(Get-MetacopyShellStates -Matrix $matrix -Layers $layers -StagingErrors $stagingErrors)
    $rows = @(Invoke-Observers -Matrix $matrix -MountPoint $MountPoint -MappedReadExePath $mappedReadExePath -StagingErrors $stagingErrors)
  }
  finally {
    Stop-Mount -Mount $mount
  }

  $table = @(Format-ResultTable -Rows $rows) + @(Get-TableFootnotes -EngineVersion $engineVersion.Full -Handshake $mount.Handshake -MountPoint $MountPoint -RebootRequired $rebootRequired -ShellStates $shellStates)
  $table | Set-Content -LiteralPath $workspacePaths.TableFile -Encoding utf8
  Write-Stage "Result table, $($rows.Count) rows, also in $($workspacePaths.TableFile)"
  Write-Host ''
  $table | Write-Output
}
catch {
  Write-Error $_
  exit 1
}
finally {
  if ($workspacePaths -and (Test-Path -LiteralPath $workspacePaths.AdapterCopy)) {
    Write-Stage "Delete the throwaway adapter copy at $($workspacePaths.AdapterCopy)"
    Remove-Item -LiteralPath $workspacePaths.AdapterCopy -Recurse -Force -ErrorAction Continue
  }
}
