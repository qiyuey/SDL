param(
    [switch]$Play,
    [int]$Round = 1,
    [int[]]$Slots = @(2, 1, 4, 3),
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Play) { throw 'Explicit -Play is required for low-amplitude physical output.' }
if ($Round -lt 1 -or @($Slots | Where-Object { $_ -lt 1 -or $_ -gt 4 }).Count) { throw 'Invalid round/depth' }
$root = Split-Path $PSScriptRoot -Parent
$out = if ($OutputDirectory) { (Resolve-Path -LiteralPath $OutputDirectory).Path } else { Join-Path $root 'build/rumble-slots' }
$exe = Join-Path $out 'testswitch2async_bench.exe'
$providers = Join-Path $out 'providers.txt'
@(
    '{36DA592D-E43A-4E28-AF6F-4BC57C5A11E8} 0xffffffffffffffff 5',
    '{30E1D284-5D88-459C-83FD-6345B39B19EC} 0xffffffffffffffff 5'
) | Set-Content -LiteralPath $providers -Encoding ascii
foreach ($depth in $Slots) {
    $name = "r$Round-s$depth"
    $base = Join-Path $out $name
    if (Test-Path "$base.log") { throw "Refusing to overwrite $base.log" }
    $session = "switch2-slots-$name"
    & logman create trace $session -o "$base.etl" -pf $providers -nb 16 256 -bs 256 -ets *> "$base-trace.log"
    if ($LASTEXITCODE) { throw "ETW start failed: $name" }
    try {
        & $exe --play $depth *> "$base.log"
        if ($LASTEXITCODE) { throw "Device test failed: $name ($LASTEXITCODE)" }
    } finally {
        & logman stop $session -ets *>> "$base-trace.log"
        if ($LASTEXITCODE) { throw "ETW stop failed: $name" }
    }
    & tracerpt "$base.etl" -o "$base-etw.xml" -of XML -y *> "$base-tracerpt.log"
    if ($LASTEXITCODE) { throw "ETW conversion failed: $name" }
    Write-Output "Completed $name"
}
