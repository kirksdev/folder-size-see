[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$output = Join-Path $buildDir 'FolderSizeExplorer.exe'
$mingw = 'C:\msys64\ucrt64\bin'

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$gcc = Join-Path $mingw 'gcc.exe'
$windres = Join-Path $mingw 'windres.exe'
if ((Test-Path -LiteralPath $gcc) -and (Test-Path -LiteralPath $windres)) {
    # The compiler's helper executables and runtime DLLs live beside gcc.exe.
    $env:Path = "$mingw;$env:Path"
    $resourceObject = Join-Path $buildDir 'app-resource.o'
    & $windres (Join-Path $projectRoot 'resource\app.rc') $resourceObject `
        "--include-dir=$(Join-Path $projectRoot 'resource')"
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

    $optimization = if ($Configuration -eq 'Release') { '-O2' } else { '-O0' }
    $symbols = if ($Configuration -eq 'Release') { '-s' } else { '-g' }
    $sources = Get-ChildItem (Join-Path $projectRoot 'src') -Filter '*.c' | ForEach-Object FullName
    & $gcc @sources $resourceObject '-std=c11' $optimization $symbols '-municode' '-mwindows' `
        '-Wall' '-Wextra' '-Wpedantic' '-Wconversion' '-DUNICODE' '-D_UNICODE' `
        '-lcomctl32' '-lshell32' '-lshlwapi' '-lole32' '-o' $output
    if ($LASTEXITCODE -ne 0) { throw 'C compilation failed.' }
    Remove-Item -LiteralPath $resourceObject -Force
} else {
    throw 'No supported C compiler was found. Install MSYS2 UCRT64 or Visual Studio Build Tools.'
}

Write-Host "Built $output"
