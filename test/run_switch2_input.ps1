param(
    [string]$Name,
    [ValidateSet('sdl', 'native', 'report09')][string]$Mode = 'sdl'
)
$ErrorActionPreference = 'Stop'
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'A simple unique capture name is required.' }
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build/switch2-input'
$base = Join-Path $out $Name
if (Test-Path "$base.log") { throw "Refusing to overwrite $base.log" }
$providers = Join-Path $out 'providers.txt'
@(
    '{36DA592D-E43A-4E28-AF6F-4BC57C5A11E8} 0xffffffffffffffff 5',
    '{30E1D284-5D88-459C-83FD-6345B39B19EC} 0xffffffffffffffff 5'
) | Set-Content -LiteralPath $providers -Encoding ascii
$session = "switch2-input-$Name"
& logman create trace $session -o "$base.etl" -pf $providers -nb 16 256 -bs 256 -ets *> "$base-trace.log"
if ($LASTEXITCODE) { throw 'Cannot start USB ETW capture' }
try {
    if ($Mode -eq 'native') {
        & (Join-Path $out 'testswitch2input_native.exe') --capture *> "$base.log"
    } elseif ($Mode -eq 'report09') {
        & (Join-Path $out 'testswitch2input.exe') --capture-09 *> "$base.log"
    } else {
        & (Join-Path $out 'testswitch2input.exe') --capture *> "$base.log"
    }
    if ($LASTEXITCODE) { throw "Input capture failed ($LASTEXITCODE)" }
} finally {
    & logman stop $session -ets *>> "$base-trace.log"
    if ($LASTEXITCODE) { throw 'Cannot stop USB ETW capture' }
}
& tracerpt "$base.etl" -o "$base-etw.xml" -of XML -y *> "$base-tracerpt.log"
if ($LASTEXITCODE) { throw 'Cannot convert USB trace' }
Get-Content "$base.log" | Select-String 'SAMPLE|SENSORS|RESULT'
