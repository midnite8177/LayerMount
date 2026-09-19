#requires -Version 7.0

$script:Origins = @(
  'upper-through-mount',
  'metacopy-shell',
  'lower-copied-up',
  'upper-before-mount',
  'lower'
)

$script:Sizes = @(0, 1, 4095, 4096, 4097, 65536, 65537, 1048576)

# The engine copies a lower file up as a metacopy shell only when the
# file is larger than its 1 MiB threshold. A 2 MiB offset clears that
# threshold for every row size, including 0.
$script:AboveMetacopyThresholdBytes = 2L * 1024 * 1024

function Get-MetacopyShellStagingOffset {
  <#
  .SYNOPSIS
    The number of bytes the script adds to a metacopy-shell case's size
    when it stages the lower file.
  #>
  $script:AboveMetacopyThresholdBytes
}

function Get-PagingReadMatrix {
  <#
  .SYNOPSIS
    Expand the origins and sizes into one case per pair.

  .DESCRIPTION
    Each case names the origin, the size in bytes, the file name the
    script stages for it, and the staged size. The staged size is the
    length on disk and the length the observers read at. It differs
    from the size only for a metacopy shell. The origin says how the
    file came to be in the overlay.
  #>
  foreach ($origin in $script:Origins) {
    foreach ($size in $script:Sizes) {
      $stagedSize = if ($origin -eq 'metacopy-shell') { $script:AboveMetacopyThresholdBytes + $size } else { $size }
      [PSCustomObject]@{
        Origin     = $origin
        Size       = $size
        FileName   = '{0}-{1}.bin' -f $origin, $size
        StagedSize = $stagedSize
      }
    }
  }
}

function Format-ShellStateFootnote {
  <#
  .SYNOPSIS
    One footnote line that says whether every metacopy-shell upper file
    was a sparse file of its staged size after staging.

  .PARAMETER States
    One object per staged metacopy-shell case with Size, StagedSize,
    Length and IsSparse. Length is $null when the upper file is missing.
  #>
  param(
    [Parameter(Mandatory)]
    [AllowEmptyCollection()]
    [array]$States
  )

  $wrong = foreach ($state in $States) {
    if ($state.IsSparse -and $state.Length -eq $state.StagedSize) { continue }
    $detail = if ($null -eq $state.Length) { 'no upper file' } else { 'length {0}, sparse={1}' -f $state.Length, $state.IsSparse }
    'size {0}: {1}' -f $state.Size, $detail
  }
  if (-not $wrong) {
    return 'After staging, every metacopy-shell upper file was a sparse file of its staged size.'
  }
  'After staging, these metacopy-shell upper files were not a sparse file of the staged size: ' + ($wrong -join '; ') + '.'
}

function Get-PatternBytes {
  <#
  .SYNOPSIS
    Build the byte pattern a staged file of the given length holds.

  .DESCRIPTION
    Byte i is a line feed when i % 64 == 63 and 0x21 + (i % 90)
    otherwise. Every byte is printable ASCII, find.exe sees a line every
    64 bytes, and a zero-filled page differs from the pattern at every
    byte.
  #>
  param(
    [Parameter(Mandatory)]
    [long]$Length
  )

  $bytes = [byte[]]::new($Length)
  for ($i = 0L; $i -lt $Length; $i++) {
    if ($i % 64 -eq 63) {
      $bytes[$i] = 0x0A
    }
    else {
      $bytes[$i] = [byte](0x21 + ($i % 90))
    }
  }

  Write-Output -NoEnumerate $bytes
}

function New-ObserverResult {
  <#
  .SYNOPSIS
    Build the object one observer's run over one file produces.

  .PARAMETER Outcome
    pass or fail.

  .PARAMETER Detail
    What went wrong on a failure, or the empty string.
  #>
  param(
    [Parameter(Mandatory)]
    [ValidateSet('pass', 'fail')]
    [string]$Outcome,

    [Parameter(Mandatory)]
    [AllowEmptyString()]
    [string]$Detail
  )

  [PSCustomObject]@{
    Outcome = $Outcome
    Detail  = $Detail
  }
}

function Get-ExpectedLineCount {
  <#
  .SYNOPSIS
    The number of lines find.exe counts in a pattern file of the given
    length: one per 64 bytes, plus one for a trailing partial line.
  #>
  param(
    [Parameter(Mandatory)]
    [long]$Length
  )

  [long][Math]::Ceiling($Length / 64.0)
}

function ConvertTo-FindResult {
  <#
  .SYNOPSIS
    Classify what find.exe /c /v "~" reported for one file.

  .DESCRIPTION
    The pattern never holds a tilde, so the inverted count is the
    number of lines find.exe read. find.exe exits 0 when it counted at
    least one line, 1 when it counted none, and 2 on an I/O error. A
    pass needs the count on stdout to match the count the file's size
    predicts. An empty file has no lines, so exit code 1 is a pass at
    size 0. A run that timed out is a failure whatever it wrote.

  .PARAMETER Capture
    The process capture: ExitCode, StdOut, StdErr and TimedOut. The
    last field of StdOut is the line count. StdErr is the detail on an
    I/O error.

  .PARAMETER Size
    The size in bytes the file was staged with.
  #>
  param(
    [Parameter(Mandatory)]
    [psobject]$Capture,

    [Parameter(Mandatory)]
    [long]$Size
  )

  if ($Capture.TimedOut) {
    return New-ObserverResult -Outcome 'fail' -Detail 'timed out'
  }

  $exitCode = [int]$Capture.ExitCode
  $stdOut = [string]$Capture.StdOut
  $message = ([string]$Capture.StdErr).Trim()
  $expected = Get-ExpectedLineCount -Length $Size

  if ($exitCode -eq 0) {
    if ($stdOut -match ':\s*(\d+)\s*$') {
      $counted = [long]$Matches[1]
      if ($counted -eq $expected) {
        return New-ObserverResult -Outcome 'pass' -Detail ''
      }
      return New-ObserverResult -Outcome 'fail' -Detail ('counted {0} lines, expected {1}' -f $counted, $expected)
    }
    return New-ObserverResult -Outcome 'fail' -Detail ('no line count in the output: ' + $stdOut.Trim())
  }
  if ($exitCode -eq 1 -and $Size -eq 0) {
    return New-ObserverResult -Outcome 'pass' -Detail ''
  }
  if ($exitCode -eq 1) {
    return New-ObserverResult -Outcome 'fail' -Detail ('no lines counted, expected {0}' -f $expected)
  }
  if ($exitCode -eq 2) {
    $detail = if ($message) { "I/O error: $message" } else { 'I/O error' }
    return New-ObserverResult -Outcome 'fail' -Detail $detail
  }

  $detail = 'exit code {0}' -f $exitCode
  if ($message) { $detail += ": $message" }
  New-ObserverResult -Outcome 'fail' -Detail $detail
}

function ConvertTo-MappedReadResult {
  <#
  .SYNOPSIS
    Classify what the mapped-read program reported for one file.

  .DESCRIPTION
    Exit code 0 is a pass. Any other exit code is a failure whose detail
    is the program's one stderr line. When the process died inside the
    mapped view there is no line, so the detail shows the exit code as
    an NTSTATUS in hex. A run that timed out is a failure whatever it
    wrote.

  .PARAMETER Capture
    The process capture: ExitCode, StdOut, StdErr and TimedOut.
  #>
  param(
    [Parameter(Mandatory)]
    [psobject]$Capture
  )

  if ($Capture.TimedOut) {
    return New-ObserverResult -Outcome 'fail' -Detail 'timed out'
  }

  $exitCode = [int]$Capture.ExitCode
  if ($exitCode -eq 0) {
    return New-ObserverResult -Outcome 'pass' -Detail ''
  }

  $message = ([string]$Capture.StdErr -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
  if ($message) {
    return New-ObserverResult -Outcome 'fail' -Detail $message.Trim()
  }

  New-ObserverResult -Outcome 'fail' -Detail ('exit 0x{0:X8}' -f $exitCode)
}

function Format-ResultTable {
  <#
  .SYNOPSIS
    Render the case results as a Markdown table, one line per element.

  .PARAMETER Rows
    Objects with Origin, Size, Find and MappedRead fields, where Find
    and MappedRead each carry Outcome and Detail.
  #>
  param(
    [Parameter(Mandatory)]
    [AllowEmptyCollection()]
    [array]$Rows
  )

  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.Add('| Origin | Size | find.exe | Mapped read | Detail |')
  $lines.Add('|---|---:|---|---|---|')

  foreach ($row in $Rows) {
    $details = [System.Collections.Generic.List[string]]::new()
    if ($row.Find.Detail) { $details.Add('find.exe: ' + $row.Find.Detail) }
    if ($row.MappedRead.Detail) { $details.Add($row.MappedRead.Detail) }
    $detail = ($details -join '; ') -replace '\|', '\|'

    $lines.Add(('| {0} | {1} | {2} | {3} | {4} |' -f
        $row.Origin, $row.Size, $row.Find.Outcome, $row.MappedRead.Outcome, $detail))
  }

  Write-Output -NoEnumerate $lines.ToArray()
}

function Set-PackageReferenceVersion {
  <#
  .SYNOPSIS
    Pin one PackageReference in a project file's text to a version.

  .PARAMETER PackageId
    The exact package id whose Version attribute changes.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$ProjectXml,

    [Parameter(Mandatory)]
    [string]$PackageId,

    [Parameter(Mandatory)]
    [string]$Version
  )

  $pattern = '(<PackageReference\s+Include="{0}"\s+Version=")[^"]*(")' -f [regex]::Escape($PackageId)
  if ($ProjectXml -notmatch $pattern) {
    throw "The project has no PackageReference to '$PackageId'."
  }

  [regex]::Replace($ProjectXml, $pattern, ('${{1}}{0}${{2}}' -f $Version))
}

function Get-EngineReferenceKind {
  <#
  .SYNOPSIS
    Say how a project file's text references the engine: through the
    package or through the engine project.

  .DESCRIPTION
    Returns PackageReference when the text holds a PackageReference to
    the package id, ProjectReference when it holds a ProjectReference
    whose Include is the project file name after the last directory
    separator, and nothing when it holds neither. A project that holds
    both counts as a package consumer.

  .PARAMETER PackageId
    The exact package id of the engine's managed wrapper.

  .PARAMETER ProjectFileName
    The file name of the engine's managed wrapper project.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$ProjectXml,

    [Parameter(Mandatory)]
    [string]$PackageId,

    [Parameter(Mandatory)]
    [string]$ProjectFileName
  )

  $packagePattern = '<PackageReference\s+Include="{0}"' -f [regex]::Escape($PackageId)
  if ($ProjectXml -match $packagePattern) {
    return 'PackageReference'
  }
  if ($ProjectXml -match (Get-ProjectReferencePattern -ProjectFileName $ProjectFileName)) {
    return 'ProjectReference'
  }
  $null
}

function Get-ProjectReferencePattern {
  <#
  .SYNOPSIS
    The regex that matches a ProjectReference whose Include names the
    given file, after any directory part with either slash.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$ProjectFileName
  )

  '(<ProjectReference\s+Include=")(?:[^"]*[\\/])?{0}(")' -f [regex]::Escape($ProjectFileName)
}

function Set-ProjectReferencePath {
  <#
  .SYNOPSIS
    Point one ProjectReference in a project file's text at a new path.

  .DESCRIPTION
    Throws when no ProjectReference names the file. The match ignores
    case, as the filesystem the project file lives on does.

  .PARAMETER ProjectFileName
    The file name the reference's Include ends with, matched after the
    last directory separator. Other ProjectReferences stay unchanged.

  .PARAMETER Path
    The path that replaces the whole Include value.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$ProjectXml,

    [Parameter(Mandatory)]
    [string]$ProjectFileName,

    [Parameter(Mandatory)]
    [string]$Path
  )

  $pattern = Get-ProjectReferencePattern -ProjectFileName $ProjectFileName
  if ($ProjectXml -notmatch $pattern) {
    throw "The project has no ProjectReference to '$ProjectFileName'."
  }

  # A MatchEvaluator inserts the path verbatim. A replacement string reads $ in the path as a substitution.
  $evaluator = [System.Text.RegularExpressions.MatchEvaluator]{
    param($match)
    $match.Groups[1].Value + $Path + $match.Groups[2].Value
  }
  [regex]::Replace($ProjectXml, $pattern, $evaluator, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

function Wait-MountPoint {
  <#
  .SYNOPSIS
    Polls every 250 ms until the mount point's root directory exists,
    and throws when it does not exist at the end of the timeout.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$MountPoint,
    [Parameter(Mandatory)]
    [TimeSpan]$Timeout
  )

  # Join-Path throws under ErrorActionPreference Stop for a drive letter that is not present yet.
  $separator = [System.IO.Path]::DirectorySeparatorChar
  $root = $MountPoint.TrimEnd($separator) + $separator
  $deadline = [DateTime]::UtcNow.Add($Timeout)
  while (-not [System.IO.Directory]::Exists($root) -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 250
  }
  if (-not [System.IO.Directory]::Exists($root)) {
    throw "The mount point '$MountPoint' is not reachable although the host adapter reported it mounted."
  }
}

Export-ModuleMember -Function Get-PagingReadMatrix, Get-MetacopyShellStagingOffset, Format-ShellStateFootnote, Get-PatternBytes, New-ObserverResult, Get-ExpectedLineCount, ConvertTo-FindResult, ConvertTo-MappedReadResult, Format-ResultTable, Set-PackageReferenceVersion, Set-ProjectReferencePath, Get-EngineReferenceKind, Wait-MountPoint
