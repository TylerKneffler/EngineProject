param(
    [Parameter(Mandatory = $true)]
    [string] $Editor,
    [Parameter(Mandatory = $true)]
    [string] $Scene,
    [ValidateSet('Scene', 'Editor')]
    [string] $Scope = 'Scene',
    [ValidateSet('DirectX11', 'DirectX12', 'Vulkan')]
    [string] $Backend = 'DirectX11',
    [ValidateRange(1, 1200)]
    [int] $Frames = 300,
    [ValidateRange(0, 1200)]
    [int] $WarmupFrames = 120,
    [ValidateRange(320, 7680)]
    [int] $Width = 1920,
    [ValidateRange(200, 4320)]
    [int] $Height = 1080,
    [string] $OutputDirectory = '.local/performance-logs',
    [string] $WorkingDirectory = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'
$editorPath = (Resolve-Path -LiteralPath $Editor).Path
$scenePath = (Resolve-Path -LiteralPath $Scene).Path
$outDir = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($outDir) | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$stem = "benchmark-$($Scope.ToLowerInvariant())-$stamp"
$phaseLog = Join-Path $outDir "$stem.jsonl"
$performanceLog = Join-Path $outDir "$stem-performance.jsonl"

# The frame profiler captures per-frame render/update data. The Performance
# recorder captures hardware samples and console output alongside those frames.
$env:ENGINE_EDITOR_AUTO_PROFILE_SCENE = $scenePath
$env:ENGINE_EDITOR_AUTO_PROFILE_FRAMES = "$Frames"
$env:ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES = "$WarmupFrames"
$env:ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES = '0'
$env:ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW = $Scope.ToLowerInvariant()
$env:ENGINE_EDITOR_FRAME_PROFILE = $phaseLog
$env:ENGINE_EDITOR_PERFORMANCE_LOG = $performanceLog
$env:ENGINE_EDITOR_BENCHMARK_SCOPE = $Scope.ToLowerInvariant()
$env:ENGINE_EDITOR_BENCHMARK_WIDTH = "$Width"
$env:ENGINE_EDITOR_BENCHMARK_HEIGHT = "$Height"

try {
    $process = Start-Process -FilePath $editorPath -ArgumentList @('--editor-renderer', $Backend) `
        -WorkingDirectory (Resolve-Path -LiteralPath $WorkingDirectory).Path -PassThru -Wait
} finally {
    'ENGINE_EDITOR_AUTO_PROFILE_SCENE', 'ENGINE_EDITOR_AUTO_PROFILE_FRAMES',
    'ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES', 'ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES',
    'ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW', 'ENGINE_EDITOR_FRAME_PROFILE',
    'ENGINE_EDITOR_PERFORMANCE_LOG', 'ENGINE_EDITOR_BENCHMARK_SCOPE',
    'ENGINE_EDITOR_BENCHMARK_WIDTH', 'ENGINE_EDITOR_BENCHMARK_HEIGHT' |
        ForEach-Object { Remove-Item "Env:$_" -ErrorAction SilentlyContinue }
}

if ($process.ExitCode -ne 0) {
    throw "Editor benchmark exited with code $($process.ExitCode). Logs: $phaseLog ; $performanceLog"
}
Write-Host "Benchmark complete ($Scope, $Backend, ${Width}x${Height})."
Write-Host "Frame capture: $phaseLog"
Write-Host "Performance log: $performanceLog"
