param(
    [Parameter(Mandatory = $false)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
}
$Version = $Version.Trim()
if ($Version.StartsWith('v', [System.StringComparison]::OrdinalIgnoreCase)) {
    $Version = $Version.Substring(1)
}
if ($Version -notmatch '^[0-9][0-9A-Za-z._-]*$') {
    throw "Invalid release version '$Version'. Use letters, numbers, dots, underscores, or hyphens."
}

$buildDirectory = Join-Path $repositoryRoot 'build\Distribution'
$releasesDirectory = Join-Path $repositoryRoot 'ReleaseBuilds'
$releaseDirectory = Join-Path $releasesDirectory ("v" + $Version)
$stagingDirectory = Join-Path $releasesDirectory (".staging-v" + $Version)
$zipPath = Join-Path $releasesDirectory ("EngineProject-v" + $Version + "-windows-x64.zip")
$binaryDirectory = Join-Path $buildDirectory 'Release'

Write-Host "Configuring portable engine release v$Version..."
& cmake -S $repositoryRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 `
    -DENGINE_PORTABLE_EXPORT=ON -DENGINE_DISTRIBUTION_BUILD=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

Write-Host 'Building Editor.exe and Game.exe...'
& cmake --build $buildDirectory --config Release --target Editor Game --parallel
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$editorExecutable = Join-Path $binaryDirectory 'Editor.exe'
$gameExecutable = Join-Path $binaryDirectory 'Game.exe'
if (!(Test-Path -LiteralPath $editorExecutable -PathType Leaf) -or
    !(Test-Path -LiteralPath $gameExecutable -PathType Leaf)) {
    throw "Expected release executables were not produced in '$binaryDirectory'."
}

if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDirectory | Out-Null

Copy-Item -LiteralPath $editorExecutable -Destination $stagingDirectory
Copy-Item -LiteralPath $gameExecutable -Destination $stagingDirectory
Get-ChildItem -LiteralPath $binaryDirectory -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stagingDirectory
}

# Runtime content plus engine sources are included because generated projects
# compile their game scripts against this packaged engine automatically.
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'Engine') `
    -Destination (Join-Path $stagingDirectory 'Engine') -Recurse
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'ProjectTemplate') `
    -Destination (Join-Path $stagingDirectory 'ProjectTemplate') -Recurse
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'CMakeLists.txt') -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'CMakePresets.json') -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'imgui.ini') -Destination $stagingDirectory

$vulkanShaders = Join-Path $buildDirectory 'VulkanShaders'
if (Test-Path -LiteralPath $vulkanShaders -PathType Container) {
    Copy-Item -LiteralPath $vulkanShaders `
        -Destination (Join-Path $stagingDirectory 'VulkanShaders') -Recurse
}

$readme = @"
EngineProject v$Version (Windows x64)

Run Editor.exe from this folder. Game.exe is the built-in standalone sandbox.
The MSVC runtime is linked statically; DirectX and Vulkan use Windows/driver
runtime components. Vulkan is optional and the editor can use DirectX 11/12.

The included Engine and ProjectTemplate folders are required runtime/tooling
content. Keep the release folder together when moving it.
"@
Set-Content -LiteralPath (Join-Path $stagingDirectory 'README.txt') `
    -Value $readme -Encoding UTF8

if (Test-Path -LiteralPath $releaseDirectory) {
    Remove-Item -LiteralPath $releaseDirectory -Recurse -Force
}
Move-Item -LiteralPath $stagingDirectory -Destination $releaseDirectory

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -LiteralPath $releaseDirectory -DestinationPath $zipPath `
    -CompressionLevel Optimal

$size = (Get-ChildItem -LiteralPath $releaseDirectory -Recurse -File |
    Measure-Object -Property Length -Sum).Sum
$sizeMb = [Math]::Round($size / 1MB, 1)
Write-Host "Release ready: $releaseDirectory ($sizeMb MB)"
Write-Host "Download archive: $zipPath"
