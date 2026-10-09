param([string]$Preset = 'windows-msvc-release', [string]$CMake = 'cmake', [string]$BuildCMakeSource)
$ErrorActionPreference = 'Stop'
Push-Location "$PSScriptRoot/.."
try {
    $bootstrap = @()
    if ($BuildCMakeSource) {
        $bootstrap += "-DFETCHCONTENT_SOURCE_DIR_NCBUILD=$BuildCMakeSource"
    }
    & $CMake --preset $Preset @bootstrap
    if ($LASTEXITCODE) {
        throw 'Configure failed'
    }
    $runner = Get-Content "o/$Preset/build-runner.txt" -Raw
    & ($runner.Trim()) -Source $PWD.Path -Preset $Preset -CMake $CMake

}
finally {
    Pop-Location
}
