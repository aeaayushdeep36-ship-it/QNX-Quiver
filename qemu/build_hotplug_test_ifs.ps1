[CmdletBinding()]
param(
    [string]$QnxBase = 'C:\Users\Lenovo\qnx800',
    [string]$OriginalIfs = 'C:\Users\Lenovo\qnxvm\output\ifs.bin',
    [string]$OutputIfs,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutputIfs) {
    $OutputIfs = Join-Path $projectRoot 'build\ifs-hotplug-test.bin'
}
$qnxHost = Join-Path $QnxBase 'host\win64\x86_64'
$qnxTarget = Join-Path $QnxBase 'target\qnx'
$dumpifs = Join-Path $qnxHost 'usr\bin\dumpifs.exe'
$mkifs = Join-Path $qnxHost 'usr\bin\mkifs.exe'
foreach ($required in @($OriginalIfs, $dumpifs, $mkifs,
        (Join-Path $qnxTarget 'x86_64\boot\sys\procnto-smp-instr'),
        (Join-Path $projectRoot 'build\quiver-pci-rm'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required test IFS input is missing: $required"
    }
}

$workRoot = Join-Path $env:TEMP ('qnxquiver-hotplug-' + [guid]::NewGuid().ToString('N'))
if ([System.IO.Path]::GetFullPath($workRoot).Contains(' ')) {
    throw "QNX mkifs linker cannot use a work path containing spaces: $workRoot"
}
$extractRoot = Join-Path $workRoot 'extract'
$sourceRoot = Join-Path $workRoot 'search'
$buildRoot = Join-Path $workRoot 'root'
New-Item -ItemType Directory -Force -Path $extractRoot, $sourceRoot, $buildRoot | Out-Null

& $dumpifs -x -d $extractRoot $OriginalIfs
if ($LASTEXITCODE -ne 0) {
    throw "dumpifs failed with exit code $LASTEXITCODE"
}
$imageRoot = Join-Path $extractRoot 'proc\boot'
$originalManifest = Join-Path $imageRoot 'build\ifs.build'
if (-not (Test-Path -LiteralPath $originalManifest)) {
    throw "Could not find the original IFS build manifest in $OriginalIfs"
}

$sourcePaths = @(
    'usr/bin/toybox', 'bin/on', 'sbin/rtc', 'bin/setfacl', 'bin/ksh',
    'bin/pidin', 'bin/mount', 'bin/umount', 'usr/bin/getconf',
    'sbin/devc-ser8250', 'bin/slog2info', 'sbin/devb-eide',
    'bin/slogger2', 'sbin/fsevmgr', 'lib/dll/pci/pci_hw-Intel_x86.so',
    'lib/dll/pci/pci_slog2.so', 'lib/dll/pci/pci_cap-0x01.so',
    'lib/dll/pci/pci_cap-0x05.so', 'lib/dll/pci/pci_cap-0x10.so',
    'lib/dll/pci/pci_cap-0x11.so', 'lib/dll/pci/pci_strings.so',
    'lib/dll/pci/pci_bkwd_compat.so', 'lib/dll/pci/pci_debug2.so',
    'lib/libpci_hw_common.so.3.0', 'sbin/pci-server', 'lib/libc.so.6',
    'usr/lib/ldqnx-64.so.2', 'lib/libgcc_s.so.1', 'lib/libm.so.3',
    'lib/libcam.so.2', 'lib/dll/io-blk.so', 'lib/dll/cam-disk.so',
    'lib/dll/fs-qnx6.so', 'lib/dll/fs-dos.so', 'lib/libslog2.so.1',
    'usr/lib/libcrypto.so.3', 'lib/libsocket.so.4', 'usr/lib/libz.so.2',
    'lib/libslog2shim.so.1', 'lib/libsecpol.so.1', 'lib/libpci.so.3.0',
    'lib/libjson.so.1', 'lib/libslog2parse.so.1', 'lib/libjail.so.1',
    'lib/libfsnotify.so.1', 'usr/lib/libqcrypto.so.1.0',
    'lib/dll/qcrypto-openssl-3.so', 'lib/libqh.so.1', 'lib/libregex.so.1',
    'lib/dll/fs-qtd.so', 'sbin/procnto-smp-instr'
)
foreach ($relativePath in $sourcePaths) {
    $name = Split-Path $relativePath -Leaf
    $sourcePath = Join-Path $imageRoot $name
    if (-not (Test-Path -LiteralPath $sourcePath) -and $name.EndsWith('.so')) {
        $versioned = Get-ChildItem -LiteralPath $imageRoot -File -Filter ($name + '.*') |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($versioned) { $sourcePath = $versioned.FullName }
    }
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Required QNX IFS file is missing: $name"
    }
    $destination = Join-Path $sourceRoot ($relativePath.Replace('/', '\'))
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destination -Force
}

$targetRoot = Join-Path $qnxTarget 'x86_64'
foreach ($relativePath in @(
        'boot/sys/multiboot.boot', 'boot/sys/startup-x86',
        'boot/sys/procnto-smp-instr', 'sbin/pci-server',
        'lib/libpci.so.3.0', 'lib/libpci_hw_common.so.3.0',
        'lib/dll/pci/pci_hw-Intel_x86.so', 'lib/dll/pci/pci_slog2.so',
        'lib/dll/pci/pci_cap-0x01.so', 'lib/dll/pci/pci_cap-0x04.so',
        'lib/dll/pci/pci_cap-0x05.so', 'lib/dll/pci/pci_cap-0x10.so',
        'lib/dll/pci/pci_cap-0x11.so', 'lib/dll/pci/pci_strings.so',
        'lib/dll/pci/pci_bkwd_compat.so', 'lib/dll/pci/pci_debug2.so',
        'lib/dll/pci/pci_server-event_handler.so',
        'lib/dll/pci/pci_server-buscfg-hotplug.so')) {
    $sourcePath = Join-Path $targetRoot ($relativePath.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Required QNX SDP 8.0 input is missing: $sourcePath"
    }
    $destination = Join-Path $sourceRoot ($relativePath.Replace('/', '\'))
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destination -Force
}
foreach ($name in @('multiboot.boot', 'startup-x86', 'procnto-smp-instr')) {
    Copy-Item -LiteralPath (Join-Path $targetRoot (Join-Path 'boot\sys' $name)) `
        -Destination (Join-Path $sourceRoot $name) -Force
}
Copy-Item -LiteralPath (Join-Path $targetRoot 'boot\sys\procnto-smp-instr') -Destination (Join-Path $sourceRoot 'procnto-smp-instr') -Force

foreach ($relativePath in @('quiver-pci-rm', 'pci_hotplug_wait.sh', 'pci_hotplug_cycle.sh')) {
    $sourcePath = switch ($relativePath) {
        'quiver-pci-rm' { Join-Path $projectRoot 'build\quiver-pci-rm' }
        default { Join-Path $projectRoot (Join-Path 'qnx' $relativePath) }
    }
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Required QNX-Quiver test component is missing: $sourcePath"
    }
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $sourceRoot $relativePath) -Force
}

# Put the original image's scripts and metadata where its buildfile expects them.
$buildFiles = Join-Path $buildRoot 'output\build'
$includeFiles = Join-Path $buildRoot 'output\inc'
$localFiles = Join-Path $buildRoot 'local'
New-Item -ItemType Directory -Force -Path $buildFiles, $includeFiles,
    (Join-Path $localFiles 'misc_files') | Out-Null
foreach ($name in @('startup.sh', 'mount_fs.sh')) {
    Copy-Item -LiteralPath (Join-Path $imageRoot $name) -Destination (Join-Path $buildFiles $name) -Force
}
foreach ($name in @('system.build', 'data.build', 'ifs.build')) {
    Copy-Item -LiteralPath (Join-Path $imageRoot (Join-Path 'build' $name)) -Destination (Join-Path $buildFiles $name) -Force
}
Copy-Item -LiteralPath (Join-Path $imageRoot 'build\options') -Destination (Join-Path $localFiles 'options') -Force
Copy-Item -LiteralPath (Join-Path $imageRoot 'build\options.inc') -Destination (Join-Path $includeFiles 'options.inc') -Force
Copy-Item -LiteralPath (Join-Path $imageRoot 'build.date') -Destination (Join-Path $buildFiles 'build.date') -Force
Copy-Item -LiteralPath (Join-Path $imageRoot 'qtd_public_key.pem') -Destination (Join-Path $localFiles 'misc_files\qtd_public_key.pem') -Force

$manifestPath = Join-Path $buildFiles 'ifs.build'
$manifest = Get-Content -LiteralPath $manifestPath -Raw
$manifest = [regex]::Replace($manifest, '(?m)^(lib/dll/pci/pci_cap-0x01\.so)\r?$', '$1' + "`r`nlib/dll/pci/pci_cap-0x04.so", 1)
$hotplugConfig = "[runtime]`r`nPCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so`r`n`r`n[buscfg]`r`nDO_BUS_CONFIG=yes`r`nPCI_SERVER_BUSCFG_MODULE=/lib/dll/pci/pci_server-buscfg-hotplug.so"
$manifest = [regex]::Replace($manifest, '\[buscfg\]\s*DO_BUS_CONFIG=no', $hotplugConfig, 1)
if ($manifest -notmatch 'PCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler\.so' -or
    $manifest -notmatch 'PCI_SERVER_BUSCFG_MODULE=/lib/dll/pci/pci_server-buscfg-hotplug\.so') {
    throw 'Could not enable the PCI hotplug modules in the test IFS config'
}
$manifest = [regex]::Replace($manifest, '(?m)^sbin/pci-server\s*$',
    '$0' + "`r`n/lib/dll/pci/pci_server-event_handler.so=lib/dll/pci/pci_server-event_handler.so`r`n/lib/dll/pci/pci_server-buscfg-hotplug.so=lib/dll/pci/pci_server-buscfg-hotplug.so`r`nquiver-pci-rm`r`npci_hotplug_wait.sh`r`npci_hotplug_cycle.sh", 1)
if ($manifest -notmatch '(?m)^quiver-pci-rm\s*$') {
    throw 'Could not add the PCI resource manager to the test IFS'
}
Set-ItemProperty -LiteralPath $manifestPath -Name IsReadOnly -Value $false
Set-Content -LiteralPath $manifestPath -Value $manifest -Encoding Ascii

$startupPath = Join-Path $buildFiles 'startup.sh'
$startup = Get-Content -LiteralPath $startupPath -Raw
$startupPattern = '(?m)^(\s*chmod a\+rx /dev/pci/server_id_1)\s*$'
if ($startup -notmatch $startupPattern) {
    throw 'Could not find the point after PCI-server startup in startup.sh'
}
$startup = [regex]::Replace($startup, $startupPattern,
    '$1' + "`r`n`r`necho `"---> Starting QNX-Quiver PCI resource manager`"`r`n /proc/boot/quiver-pci-rm &`r`n /proc/boot/pci_hotplug_cycle.sh 60 &", 1)
Set-ItemProperty -LiteralPath $startupPath -Name IsReadOnly -Value $false
$startup = $startup.Replace("`r`n", "`n").Replace("`r", "`n")
[System.IO.File]::WriteAllText($startupPath, $startup, [System.Text.Encoding]::ASCII)

$env:QNX_HOST = $qnxHost
$env:QNX_TARGET = $qnxTarget
$env:PROCESSOR = 'x86_64'
$env:MKFS_PATH = $sourceRoot
$imageInWork = Join-Path $workRoot 'ifs-hotplug-test.bin'
Push-Location $buildRoot
try {
    & $mkifs -v 'output\build\ifs.build' '.' $imageInWork
    if ($LASTEXITCODE -ne 0) { throw "mkifs failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$verifyDir = Join-Path $workRoot 'verify'
New-Item -ItemType Directory -Force -Path $verifyDir | Out-Null
& $dumpifs -d $verifyDir -f '/proc/boot/pci_server.cfg' -f '/proc/boot/startup.sh' $imageInWork
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the built IFS (exit $LASTEXITCODE)" }
$configPath = Join-Path $verifyDir 'proc\boot\pci_server.cfg'
$startupCheckPath = Join-Path $verifyDir 'proc\boot\startup.sh'
$config = Get-Content -LiteralPath $configPath -Raw
$startupCheck = Get-Content -LiteralPath $startupCheckPath -Raw
foreach ($required in @('PCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so',
        'DO_BUS_CONFIG=yes', 'PCI_SERVER_BUSCFG_MODULE=/lib/dll/pci/pci_server-buscfg-hotplug.so')) {
    if ($config -notmatch [regex]::Escape($required)) {
        throw "Built IFS config is missing: $required"
    }
}
foreach ($required in @('/proc/boot/quiver-pci-rm &', '/proc/boot/pci_hotplug_cycle.sh 60 &')) {
    if (-not $startupCheck.Contains($required)) {
        throw "Built IFS startup is missing: $required"
    }
}

$outputParent = Split-Path $OutputIfs -Parent
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
Copy-Item -LiteralPath $imageInWork -Destination $OutputIfs -Force
Write-Output "PASS: built and inspected software-hotplug test IFS: $OutputIfs"
Write-Output "Work files: $workRoot"
