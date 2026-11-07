param([switch]$Smoke)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    foreach ($configuration in @('Debug', 'Release')) {
        & "$PSScriptRoot/build.ps1" -Configuration $configuration
        if ($Smoke) {
            $exe = Join-Path $repo "out/build/windows-vs2022-x64/$configuration/Shirane.exe"
            $process = Start-Process -FilePath $exe -ArgumentList '--smoke-test' -WorkingDirectory $repo -WindowStyle Hidden -PassThru
            if (-not $process.WaitForExit(30000)) {
                $process.Kill()
                throw "$configuration startup check exceeded 30 seconds"
            }
            if ($process.ExitCode -ne 0) { throw "$configuration startup check failed: $($process.ExitCode)" }
        }
    }
} finally { Pop-Location }
