$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or !$IsWindows) {
    throw 'VEConfigure can only be installed by this workflow on a disposable GitHub-hosted Windows runner.'
}

$installerUri = 'https://www.victronenergy.com/Executables/VEConfig/VECSetup_B.exe'
$installerHash = 'BA1027DFF4428E9358167D9EB108CF5BC48470DD8B6745652326308D843F0A5B'
$installer = Join-Path $env:RUNNER_TEMP 'VECSetup_B.exe'
$directory = Join-Path $env:RUNNER_TEMP 'brain-zero-veconfigure'
$exe = Join-Path $directory 'VEConfig.exe'

Invoke-WebRequest -Uri $installerUri -OutFile $installer -MaximumRedirection 0 -TimeoutSec 40
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash -ne $installerHash) {
    throw 'Victron changed the installer. Review and pin the new download before running it.'
}

$arguments = @('/SP-', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS',
    '/NOICONS', '/TASKS=""', "/DIR=""$directory""")
$process = Start-Process -FilePath $installer -ArgumentList $arguments -WindowStyle Hidden -PassThru
if (!$process.WaitForExit(60000)) {
    Stop-Process -Id $process.Id -Force
    throw 'VEConfigure installation timed out.'
}
if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "VEConfigure installer did not produce the expected executable (exit $($process.ExitCode))."
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash -ne 'D979D4F7AD111F2FE9BB3C504EC74C55857A124373DD202AF1454750B43FCA2F') {
    throw 'The installed VEConfigure executable does not match the reference binary.'
}

Set-NetFirewallProfile -Profile Domain,Private,Public -Enabled True
New-NetFirewallRule -DisplayName 'brain_zero VEConfigure offline' -Direction Outbound -Action Block -Program $exe -Profile Any | Out-Null
Add-Content -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Value "VE_CONFIG_EXE=$exe"
