param(
    [switch]$PackageOnly
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$extensionDirectory = $PSScriptRoot
$engineRoot = [System.IO.Path]::GetFullPath((Join-Path $extensionDirectory '..\..\..'))
$manifest = Get-Content -LiteralPath (Join-Path $extensionDirectory 'package.json') -Raw |
    ConvertFrom-Json
$outputDirectory = Join-Path $engineRoot '.local\vscode-bridge'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$vsixPath = Join-Path $outputDirectory ("$($manifest.name)-$($manifest.version).vsix")
if (Test-Path -LiteralPath $vsixPath) {
    Remove-Item -LiteralPath $vsixPath -Force
}

function Add-VsixText {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string]$Name,
        [string]$Contents
    )
    $entry = $Archive.CreateEntry($Name)
    $stream = $entry.Open()
    try {
        $writer = [System.IO.StreamWriter]::new($stream,
            [System.Text.UTF8Encoding]::new($false))
        try { $writer.Write($Contents) }
        finally { $writer.Dispose() }
    }
    finally { $stream.Dispose() }
}

$contentTypes = @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json"/>
  <Default Extension="js" ContentType="application/javascript"/>
  <Default Extension="vsixmanifest" ContentType="text/xml"/>
</Types>
'@
$vsixManifest = @"
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="$($manifest.name)" Version="$($manifest.version)" Publisher="$($manifest.publisher)"/>
    <DisplayName>$($manifest.displayName)</DisplayName>
    <Description xml:space="preserve">$($manifest.description)</Description>
    <Tags></Tags>
    <Categories>Debuggers</Categories>
    <GalleryFlags>Private</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="$($manifest.engines.vscode)"/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="ms-vscode.cpptools"/>
    </Properties>
  </Metadata>
  <Installation><InstallationTarget Id="Microsoft.VisualStudio.Code"/></Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>
  </Assets>
</PackageManifest>
"@

$archive = [System.IO.Compression.ZipFile]::Open($vsixPath,
    [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $packageJson = Get-Content -LiteralPath (Join-Path $extensionDirectory 'package.json') -Raw
    $extensionJs = Get-Content -LiteralPath (Join-Path $extensionDirectory 'extension.js') -Raw
    Add-VsixText $archive '[Content_Types].xml' $contentTypes
    Add-VsixText $archive 'extension.vsixmanifest' $vsixManifest
    Add-VsixText $archive 'extension/package.json' $packageJson
    Add-VsixText $archive 'extension/extension.js' $extensionJs
}
finally { $archive.Dispose() }

Write-Host "Packaged $vsixPath"
if ($PackageOnly) { return }

$code = Get-Command code.cmd -ErrorAction Stop
& $code.Source --install-extension $vsixPath --force
if ($LASTEXITCODE -ne 0) {
    throw "VS Code extension installation failed with exit code $LASTEXITCODE."
}
Write-Host 'Reopen the VS Code workspace to activate Engine Editor Auto Attach.'
