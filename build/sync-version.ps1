#requires -Version 7.0
<#
.SYNOPSIS
  Propagate the version from build/LayerMount.Version.csproj (the
  Versionize anchor) into version.props at the repo root.

.DESCRIPTION
  Versionize edits <Version> in the anchor csproj. The rest of the
  build (native macro codegen, managed assemblies, NuGet packaging)
  reads from version.props, which carries separate Major/Minor/Patch/
  Suffix nodes plus a derived <Version>. This script parses the anchor
  csproj <Version>, extracts SemVer parts, and rewrites version.props.

  Idempotent. Safe to run any number of times.

.PARAMETER RepoRoot
  Repo root. Defaults to the parent of this script's directory.
#>

[CmdletBinding()]
param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$anchorPath  = Join-Path $RepoRoot 'build/LayerMount.Version.csproj'
$propsPath   = Join-Path $RepoRoot 'version.props'

if (-not (Test-Path -LiteralPath $anchorPath)) {
  throw "Version anchor not found at $anchorPath"
}
if (-not (Test-Path -LiteralPath $propsPath)) {
  throw "version.props not found at $propsPath"
}

[xml]$anchor = Get-Content -LiteralPath $anchorPath
$anchorVersionNode = $anchor.SelectSingleNode('//Version')
if (-not $anchorVersionNode) {
  throw "build/LayerMount.Version.csproj does not contain a <Version> element"
}

$rawVersion = $anchorVersionNode.InnerText.Trim()
if ($rawVersion -notmatch '^(?<maj>\d+)\.(?<min>\d+)\.(?<pat>\d+)(?:-(?<suf>[0-9A-Za-z.\-]+))?$') {
  throw "Anchor <Version> '$rawVersion' is not a valid SemVer string"
}

$major  = $Matches['maj']
$minor  = $Matches['min']
$patch  = $Matches['pat']
$suffix = if ($Matches.ContainsKey('suf')) { $Matches['suf'] } else { '' }

[xml]$props = Get-Content -LiteralPath $propsPath
$ns = $props.DocumentElement.NamespaceURI

function Set-Node([xml]$doc, [string]$xpath, [string]$value) {
  $node = $doc.SelectSingleNode($xpath)
  if (-not $node) {
    throw "version.props missing element at xpath: $xpath"
  }
  $node.InnerText = $value
}

Set-Node $props '//LayerMountVersionMajor'  $major
Set-Node $props '//LayerMountVersionMinor'  $minor
Set-Node $props '//LayerMountVersionPatch'  $patch
Set-Node $props '//LayerMountVersionSuffix' $suffix

$settings = New-Object System.Xml.XmlWriterSettings
$settings.Indent              = $true
$settings.IndentChars         = '  '
$settings.NewLineChars        = "`n"
$settings.NewLineHandling     = [System.Xml.NewLineHandling]::Replace
$settings.OmitXmlDeclaration  = $false
$settings.Encoding            = [System.Text.UTF8Encoding]::new($false)

$writer = [System.Xml.XmlWriter]::Create($propsPath, $settings)
try { $props.Save($writer) } finally { $writer.Dispose() }

Write-Host "version.props updated: $major.$minor.$patch$(if($suffix){"-$suffix"}else{''})"
