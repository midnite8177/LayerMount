#requires -Version 7.0

function ConvertTo-GitignoreSegmentRegex {
  param(
    [string]$Segment
  )

  $result = [System.Text.StringBuilder]::new()
  $i = 0
  while ($i -lt $Segment.Length) {
    $char = $Segment[$i]
    if ($char -eq '*') {
      [void]$result.Append('[^/]*')
      $i++
    }
    elseif ($char -eq '?') {
      [void]$result.Append('[^/]')
      $i++
    }
    elseif ($char -eq '[') {
      $close = $Segment.IndexOf(']', $i + 1)
      if ($close -lt 0) {
        [void]$result.Append([regex]::Escape($char))
        $i++
      }
      else {
        $classBody = $Segment.Substring($i + 1, $close - $i - 1)
        if ($classBody.StartsWith('!')) {
          $classBody = '^' + $classBody.Substring(1)
        }
        [void]$result.Append('[' + $classBody + ']')
        $i = $close + 1
      }
    }
    else {
      [void]$result.Append([regex]::Escape([string]$char))
      $i++
    }
  }

  $result.ToString()
}

function ConvertTo-GitignoreRegex {
  param(
    [string]$Pattern
  )

  $globstarToken = '@@GLOBSTAR@@'
  $segments = $Pattern -split '/'
  $segmentRegexes = foreach ($segment in $segments) {
    if ($segment -eq '**') {
      $globstarToken
    }
    else {
      ConvertTo-GitignoreSegmentRegex -Segment $segment
    }
  }

  $joined = $segmentRegexes -join '/'
  $joined = $joined -replace "^$globstarToken/", '(?:.*/)?'
  $joined = $joined -replace "/$globstarToken/", '/(?:.*/)?'
  $joined = $joined -replace "/$globstarToken$", '(?:/.*)?'
  $joined = $joined -replace "^$globstarToken$", '.*'

  $joined
}

function New-SyncIgnorePattern {
  <#
  .SYNOPSIS
    Build a sync manifest ignore-pattern record.

  .PARAMETER Pattern
    The gitignore pattern text, with any leading or trailing slash
    already stripped.

  .PARAMETER Scope
    The repository-relative directory the pattern applies under, or an
    empty string for the repository root.

  .PARAMETER DirectoryOnly
    The pattern matched a directory-only line (one that ended in `/`).

  .PARAMETER Anchored
    The pattern matches relative to its scope only, not at any depth
    below it.

  .PARAMETER Negate
    The pattern un-ignores a path an earlier pattern ignored.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$Pattern,

    [Parameter(Mandatory)]
    [AllowEmptyString()]
    [string]$Scope,

    [switch]$DirectoryOnly,
    [switch]$Anchored,
    [switch]$Negate
  )

  [PSCustomObject]@{
    Pattern       = $Pattern
    Scope         = $Scope
    DirectoryOnly = [bool]$DirectoryOnly
    Anchored      = [bool]$Anchored
    Negate        = [bool]$Negate
  }
}

function Test-UnderGitDir {
  <#
  .SYNOPSIS
    Decide whether a relative path falls under the .git directory.

  .PARAMETER RelativePath
    The path to test, POSIX-separated and relative to the repository root.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$RelativePath
  )

  $RelativePath -eq '.git' -or $RelativePath.StartsWith('.git/')
}

function Test-PathIgnoredBySyncManifest {
  <#
  .SYNOPSIS
    Decide whether a sync manifest's ignore patterns cover a path.

  .DESCRIPTION
    Walks the ignore patterns in order and applies git's own rule: the
    last pattern that matches wins, and a negated pattern un-ignores the
    path. A path with no matching pattern is not ignored.

  .PARAMETER RelativePath
    The path to test, POSIX-separated and relative to the source root.

  .PARAMETER IgnorePatterns
    The ordered pattern records from a sync manifest, each with
    Pattern, Scope, DirectoryOnly, Anchored, and Negate fields.
  #>
  param(
    [Parameter(Mandatory)]
    [string]$RelativePath,

    [Parameter(Mandatory)]
    [AllowEmptyCollection()]
    [array]$IgnorePatterns
  )

  $matchedAny = $false
  $ignored = $false

  foreach ($record in $IgnorePatterns) {
    $scope = $record.Scope

    if ([string]::IsNullOrEmpty($scope)) {
      $scopedPath = $RelativePath
    }
    elseif ($RelativePath.StartsWith($scope + '/', [System.StringComparison]::OrdinalIgnoreCase)) {
      $scopedPath = $RelativePath.Substring($scope.Length + 1)
    }
    else {
      continue
    }

    $core = ConvertTo-GitignoreRegex -Pattern $record.Pattern
    $prefix = if ($record.Anchored) { '' } else { '(?:.*/)?' }
    $suffix = if ($record.DirectoryOnly) { '/.*' } else { '(?:/.*)?' }
    $regexText = '^' + $prefix + $core + $suffix + '$'

    if ($scopedPath -imatch $regexText) {
      $matchedAny = $true
      $ignored = -not $record.Negate
    }
  }

  $matchedAny -and $ignored
}

Export-ModuleMember -Function New-SyncIgnorePattern, Test-UnderGitDir, Test-PathIgnoredBySyncManifest
