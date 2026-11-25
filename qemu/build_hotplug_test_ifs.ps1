[CmdletBinding()]
param(
    [string]$QnxBase = 'C:\Users\Lenovo\qnx800',
    [string]$OriginalIfs = 'C:\Users\Lenovo\qnxvm\output\ifs.bin',
    [string]$OutputIfs,
    [switch]$EventHandlerOnly,
    [switch]$BaselinePciServer,
    [switch]$IncludeServices,
    [switch]$SupervisorMode,
    [switch]$PciOnly,
    [switch]$Buscfg2,
    [string]$ActivePolicySource,
    [switch]$PolicySourceIncludesQuiver,
    [string]$PciServerType = 'pci_server_t',
    [string]$QuiverManagerType = 'quiver_pci_t',
    [string]$DeniedClientType = 'quiver_denied_client_t',
    [switch]$SecpolGenerate,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
if ($SecpolGenerate -and $ActivePolicySource) {
    throw '-SecpolGenerate cannot be combined with -ActivePolicySource'
}
$buscfgModule = if ($Buscfg2) {
    '/lib/dll/pci/pci_server-buscfg2-hotplug.so'
} else {
    '/lib/dll/pci/pci_server-buscfg-hotplug.so'
}
if ($Buscfg2 -and ($EventHandlerOnly -or $BaselinePciServer)) {
    throw '-Buscfg2 requires the full hotplug test profile'
}
if ($SupervisorMode -and (-not $IncludeServices -or $BaselinePciServer)) {
    throw '-SupervisorMode requires -IncludeServices and the full test PCI profile'
}
if ($PciOnly -and ($IncludeServices -or $SupervisorMode -or $BaselinePciServer -or $Buscfg2)) {
    throw '-PciOnly requires the standard hotplug PCI profile without Quiver services'
}
if ($ActivePolicySource -and ($BaselinePciServer -or $PciOnly -or $EventHandlerOnly)) {
    throw '-ActivePolicySource requires the full test PCI profile with the resident Quiver manager or supervisor'
}
foreach ($typeName in @($PciServerType, $QuiverManagerType, $DeniedClientType)) {
    if ($typeName -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
        throw "Invalid security policy type name: $typeName"
    }
}
if ($PciServerType -eq $QuiverManagerType -or
    $DeniedClientType -in @($PciServerType, $QuiverManagerType)) {
    throw 'PCI server, Quiver manager, and denied-client tests must use distinct policy types'
}
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutputIfs) {
    $OutputIfs = Join-Path $projectRoot 'build\ifs-hotplug-test.bin'
}
$qnxHost = Join-Path $QnxBase 'host\win64\x86_64'
$qnxTarget = Join-Path $QnxBase 'target\qnx'
$env:QNX_HOST = $qnxHost
$env:QNX_TARGET = $qnxTarget
$env:PROCESSOR = 'x86_64'
$dumpifs = Join-Path $qnxHost 'usr\bin\dumpifs.exe'
$mkifs = Join-Path $qnxHost 'usr\bin\mkifs.exe'
$qcc = Join-Path $qnxHost 'usr\bin\qcc.exe'
$pciProbe = Join-Path $projectRoot 'build\quiver-pci-db-probe'
if (-not (Test-Path -LiteralPath $pciProbe)) {
    & $qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -O2 `
        -I (Join-Path $projectRoot 'include') -o $pciProbe `
        (Join-Path $projectRoot 'tests\qnx_pci_db_probe.c') -lpci
    if ($LASTEXITCODE -ne 0) {
        throw "Could not build the QNX PCI database probe (exit $LASTEXITCODE)"
    }
}
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
        'boot/sys/multiboot.boot', 'boot/sys/startup-x86', 'bin/secpolpush',
        'boot/sys/procnto-smp-instr', 'sbin/pci-server',
        'lib/libpci.so.3.0', 'lib/libpci_hw_common.so.3.0',
        'lib/dll/pci/pci_hw-Intel_x86.so', 'lib/dll/pci/pci_slog2.so',
        'lib/dll/pci/pci_cap-0x01.so', 'lib/dll/pci/pci_cap-0x04.so',
        'lib/dll/pci/pci_cap-0x05.so', 'lib/dll/pci/pci_cap-0x10.so',
        'lib/dll/pci/pci_cap-0x11.so', 'lib/dll/pci/pci_strings.so',
        'lib/dll/pci/pci_bkwd_compat.so', 'lib/dll/pci/pci_debug2.so',
        'lib/dll/pci/pci_server-event_handler.so',
        'lib/dll/pci/pci_server-buscfg-hotplug.so',
        'lib/dll/pci/pci_server-buscfg2-hotplug.so',
        'bin/secpolgenerate',
        'bin/secpolmonitor',
        'usr/lib/libncurses.so.6', 'usr/lib/libpanelw.so.6',
        'usr/sbin/pci-tool')) {
    $sourcePath = Join-Path $targetRoot ($relativePath.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Required QNX SDP 8.0 input is missing: $sourcePath"
    }
    $destination = Join-Path $sourceRoot ($relativePath.Replace('/', '\'))
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destination -Force
}
$terminfoSource = Join-Path $qnxTarget 'usr\share\terminfo\q\qansi'
if (-not (Test-Path -LiteralPath $terminfoSource)) {
    throw "Required QNX terminfo entry is missing: $terminfoSource"
}
$terminfoDestination = Join-Path $sourceRoot 'usr\share\terminfo\q\qansi'
New-Item -ItemType Directory -Force -Path (Split-Path $terminfoDestination -Parent) | Out-Null
Copy-Item -LiteralPath $terminfoSource -Destination $terminfoDestination -Force
foreach ($name in @('multiboot.boot', 'startup-x86', 'procnto-smp-instr')) {
    Copy-Item -LiteralPath (Join-Path $targetRoot (Join-Path 'boot\sys' $name)) `
        -Destination (Join-Path $sourceRoot $name) -Force
}
Copy-Item -LiteralPath (Join-Path $targetRoot 'boot\sys\procnto-smp-instr') -Destination (Join-Path $sourceRoot 'procnto-smp-instr') -Force

$testComponents = if ($BaselinePciServer -or $PciOnly) { @() } else { @('quiver-pci-rm', 'quiver-pci-db-probe', 'pci_hotplug_wait.sh', 'pci_hotplug_cycle.sh') }
$policyProbeComponents = if ($ActivePolicySource) {
    @('quiver-client-probe', 'quiver-client-denied-probe')
} else { @() }
$serviceComponents = if ($IncludeServices) {
    @('quiver-client-probe', 'dev-can-quiver', 'quiver-can-probe', 'quiver-can-saturation-probe',
      'quiver-imu', 'quiver-actuator', 'quiver-trace',
      'quiver-controller', 'quiver-supervisor')
} else { @() }
$testComponents += $serviceComponents
$testComponents += $policyProbeComponents
if ($ActivePolicySource) {
    $deniedProbeSource = Join-Path $projectRoot 'qnx\dev-quiver\denied_client_probe.c'
    $deniedProbeBuild = Join-Path $projectRoot 'build\quiver-client-denied-probe'
    if (-not (Test-Path -LiteralPath $deniedProbeBuild)) {
        $deniedProbeBuild = Join-Path $workRoot 'quiver-client-denied-probe'
        $deniedProbeCompileSource = Join-Path $sourceRoot 'quiver-client-denied-probe.c'
        Copy-Item -LiteralPath $deniedProbeSource -Destination $deniedProbeCompileSource -Force
        & $qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -O2 `
            -o $deniedProbeBuild $deniedProbeCompileSource
        if ($LASTEXITCODE -ne 0) {
            throw "Could not build the QNX denied-client policy probe (exit $LASTEXITCODE)"
        }
    }
}
foreach ($relativePath in $testComponents) {
    $sourcePath = switch ($relativePath) {
        'quiver-pci-rm' { Join-Path $projectRoot 'build\quiver-pci-rm' }
        'quiver-pci-db-probe' { Join-Path $projectRoot 'build\quiver-pci-db-probe' }
        'quiver-client-probe' { Join-Path $projectRoot 'build\quiver-client-probe' }
        'quiver-client-denied-probe' { $deniedProbeBuild }
        { $_ -in $serviceComponents } { Join-Path $projectRoot (Join-Path 'build' $relativePath) }
        default { Join-Path $projectRoot (Join-Path 'qnx' $relativePath) }
    }
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Required QNX-Quiver test component is missing: $sourcePath"
    }
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $sourceRoot $relativePath) -Force
    if ($relativePath.EndsWith('.sh')) {
        $scriptPath = Join-Path $sourceRoot $relativePath
        $scriptText = [System.IO.File]::ReadAllText($scriptPath)
        $scriptText = $scriptText.Replace("`r`n", "`n").Replace("`r", "`n")
        [System.IO.File]::WriteAllText($scriptPath, $scriptText, [System.Text.Encoding]::ASCII)
    }
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
$hotplugConfig = if ($BaselinePciServer) {
    "[buscfg]`r`nDO_BUS_CONFIG=no"
} elseif ($EventHandlerOnly) {
    "[runtime]`r`nPCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so`r`n`r`n[buscfg]`r`nDO_BUS_CONFIG=no"
} else {
    "[runtime]`r`nPCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so`r`n`r`n[buscfg]`r`nDO_BUS_CONFIG=yes`r`nPCI_SERVER_BUSCFG_MODULE=$buscfgModule"
}
$manifest = [regex]::Replace($manifest, '\[buscfg\]\s*DO_BUS_CONFIG=no', $hotplugConfig, 1)
if ((-not $BaselinePciServer -and $manifest -notmatch 'PCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler\.so') -or
    (-not $EventHandlerOnly -and -not $BaselinePciServer -and
     $manifest -notmatch ('PCI_SERVER_BUSCFG_MODULE=' + [regex]::Escape($buscfgModule)))) {
    throw 'Could not enable the PCI hotplug modules in the test IFS config'
}
$moduleManifest = if ($BaselinePciServer) { '' } else { '/lib/dll/pci/pci_server-event_handler.so=lib/dll/pci/pci_server-event_handler.so' }
if (-not $EventHandlerOnly -and -not $BaselinePciServer) {
    $moduleName = Split-Path $buscfgModule -Leaf
    $moduleManifest += "`r`n$buscfgModule=lib/dll/pci/$moduleName"
}
if (-not $BaselinePciServer) {
    $moduleManifest += "`r`n[+raw] usr/sbin/pci-tool`r`nusr/lib/libncurses.so.6`r`nusr/lib/libpanelw.so.6`r`nterminfo/q/qansi=usr/share/terminfo/q/qansi"
    if (-not $PciOnly) {
        $moduleManifest += "`r`nquiver-pci-rm`r`nquiver-pci-db-probe`r`npci_hotplug_wait.sh`r`npci_hotplug_cycle.sh"
    }
    foreach ($component in ($serviceComponents + $policyProbeComponents | Select-Object -Unique)) {
        $moduleManifest += "`r`n$component"
    }
}
if ($ActivePolicySource) {
    $policyPath = (Resolve-Path -LiteralPath $ActivePolicySource).Path
    $policyText = Get-Content -LiteralPath $policyPath -Raw
    if ($policyText -notmatch ('(?m)^\s*type\s+' + [regex]::Escape($PciServerType) + '\s*;')) {
        throw "The full policy source must declare PCI-server type $PciServerType; set -PciServerType if the image uses another name"
    }
    $quiverTypeDeclared = $policyText -match ('(?m)^\s*type\s+' + [regex]::Escape($QuiverManagerType) + '\s*;')
    if ($PolicySourceIncludesQuiver -and -not $quiverTypeDeclared) {
        throw "-PolicySourceIncludesQuiver was set, but the source does not declare $QuiverManagerType"
    }
    if (-not $PolicySourceIncludesQuiver -and $quiverTypeDeclared) {
        throw "The policy source already declares Quiver manager type $QuiverManagerType; pass -PolicySourceIncludesQuiver or use the target policy before Quiver integration"
    }
    if ($policyText -match ('(?m)^\s*type\s+' + [regex]::Escape($DeniedClientType) + '\s*;')) {
        throw "The policy source already declares the negative-test type $DeniedClientType; select an unused name with -DeniedClientType"
    }
    $secpolcompile = Join-Path $qnxHost 'usr\bin\secpolcompile.exe'
    if (-not (Test-Path -LiteralPath $secpolcompile)) {
        throw "QNX secpolcompile is missing: $secpolcompile"
    }
    $policyBinary = Join-Path $workRoot 'secpol.bin'
    $policyInputs = @($policyPath)
    if (-not $PolicySourceIncludesQuiver) {
        $quiverPolicySource = Join-Path $projectRoot 'qnx\security\qnx_quiver.policy'
        $quiverPolicy = Join-Path $workRoot 'qnx_quiver.policy'
        $quiverPolicyText = [System.IO.File]::ReadAllText($quiverPolicySource)
        if ($QuiverManagerType -ne 'quiver_pci_t') {
            $quiverPolicyText = $quiverPolicyText.Replace('quiver_pci_t', $QuiverManagerType)
        }
        [System.IO.File]::WriteAllText($quiverPolicy, $quiverPolicyText,
            [System.Text.Encoding]::ASCII)
        $policyInputs += $quiverPolicy
    }
    $pciRule = "allow $QuiverManagerType $PciServerType : channel connect;"
    $pciRulePattern = '(?m)^\s*allow\s+' + [regex]::Escape($QuiverManagerType) +
        '\s+' + [regex]::Escape($PciServerType) + '\s*:\s*channel\s+connect\s*;\s*$'
    $policyOverlay = Join-Path $workRoot 'qnx_quiver_policy_overlay.policy'
    $overlayRules = @()
    if ($policyText -notmatch $pciRulePattern) {
        $overlayRules += $pciRule
    }
    $overlayRules += @(
        'allow quiver_can_client_t self:ability { pathspace prot_exec };',
        'allow_attach quiver_can_client_t /proc/boot/quiver-client-probe quiver_can_client_t;',
        "type $DeniedClientType;",
        "allow $DeniedClientType self:ability { pathspace prot_exec };",
        "allow_attach $DeniedClientType /proc/boot/quiver-client-denied-probe $DeniedClientType;"
    )
    Set-Content -LiteralPath $policyOverlay -Value $overlayRules -Encoding Ascii
    $policyInputs += $policyOverlay
    & $secpolcompile -o $policyBinary @policyInputs
    if ($LASTEXITCODE -ne 0) {
        throw "secpolcompile rejected the active policy (exit $LASTEXITCODE)"
    }
    Copy-Item -LiteralPath $policyBinary -Destination (Join-Path $sourceRoot 'secpol.bin') -Force
    $moduleManifest += "`r`nsecpol.bin`r`nbin/secpolpush=bin/secpolpush`r`nbin/secpolmonitor=bin/secpolmonitor"
} elseif ($SecpolGenerate) {
    $moduleManifest += "`r`nsecpolgenerate=bin/secpolgenerate`r`nbin/secpolmonitor=bin/secpolmonitor"
}
$manifest = [regex]::Replace($manifest, '(?m)^sbin/pci-server\s*$', '$0' + "`r`n$moduleManifest", 1)
if (-not $BaselinePciServer -and -not $PciOnly -and $manifest -notmatch '(?m)^quiver-pci-rm\s*$') {
    throw 'Could not add the PCI resource manager to the test IFS'
}
Set-ItemProperty -LiteralPath $manifestPath -Name IsReadOnly -Value $false
Set-Content -LiteralPath $manifestPath -Value $manifest -Encoding Ascii

$startupPath = Join-Path $buildFiles 'startup.sh'
$startup = Get-Content -LiteralPath $startupPath -Raw
$startup = [regex]::Replace($startup, '(?m)^(#!.*)$', '$1' + "`r`nexport TERMINFO=/proc/boot/terminfo", 1)
$policyStartupPattern = '(?m)^\s*slogger2\s+-U\s+'
if ($ActivePolicySource) {
    if ($startup -notmatch $policyStartupPattern) {
        throw 'Could not find the first persistent service startup location for secpolpush'
    }
    $startup = [regex]::Replace($startup, $policyStartupPattern,
        "/proc/boot/bin/secpolpush`r`n`$0", 1)
    $pciStartPattern = '(?m)^\s*pci-server\s+--aspace-enable\s+--config=/proc/boot/pci_server\.cfg\s*$'
    if ($startup -notmatch $pciStartPattern) {
        throw 'Could not find the PCI-server startup command for typed policy launch'
    }
    $startup = [regex]::Replace($startup, $pciStartPattern,
        " on -T $PciServerType pci-server --aspace-enable --config=/proc/boot/pci_server.cfg", 1)
} elseif ($SecpolGenerate) {
    if ($startup -notmatch $policyStartupPattern) {
        throw 'Could not find the first persistent service startup location for secpolgenerate'
    }
    $startup = [regex]::Replace($startup, $policyStartupPattern,
        "/proc/boot/secpolgenerate -N -u -f 1`r`nwaitfor /dev/secpolgenerate/policy`r`n`$0", 1)
}
$startupPattern = '(?m)^(\s*chmod a\+rx /dev/pci/server_id_1)\s*$'
if ($startup -notmatch $startupPattern) {
    throw 'Could not find the point after PCI-server startup in startup.sh'
}
if (-not $BaselinePciServer -and -not $PciOnly) {
    if ($SupervisorMode) {
        $supervisorLaunch = if ($ActivePolicySource) {
            'on -T quiver_supervisor_t /proc/boot/quiver-supervisor /proc/boot &'
        } else {
            '/proc/boot/quiver-supervisor /proc/boot &'
        }
        $startup = [regex]::Replace($startup, $startupPattern,
            '$1' + "`r`n`r`necho `"---> Starting QNX-Quiver supervisor`"`r`n $supervisorLaunch", 1)
    } else {
        $managerLaunch = if ($ActivePolicySource) {
            "on -T $QuiverManagerType /proc/boot/quiver-pci-rm &"
        } else {
            '/proc/boot/quiver-pci-rm &'
        }
        $startup = [regex]::Replace($startup, $startupPattern,
            '$1' + "`r`n`r`necho `"---> Starting QNX-Quiver PCI resource manager`"`r`n $managerLaunch", 1)
    }
}
if ($PciOnly) {
    $pciServerPattern = '(?m)^\s*pci-server --aspace-enable --config=/proc/boot/pci_server\.cfg\s*$'
    if ($startup -notmatch $pciServerPattern) {
        throw 'Could not find pci-server startup command for PCI trace profile'
    }
    $startup = [regex]::Replace($startup, $pciServerPattern,
        ' PCI_BASE_VERBOSITY=5 pci-server -vv --aspace-enable --config=/proc/boot/pci_server.cfg', 1)
}
Set-ItemProperty -LiteralPath $startupPath -Name IsReadOnly -Value $false
$startup = $startup.Replace("`r`n", "`n").Replace("`r", "`n")
[System.IO.File]::WriteAllText($startupPath, $startup, [System.Text.Encoding]::ASCII)

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
$verifyArgs = @('-d', $verifyDir, '-f', '/proc/boot/pci_server.cfg',
    '-f', '/proc/boot/startup.sh')
if ($ActivePolicySource) {
    $verifyArgs += @('-f', '/proc/boot/secpol.bin',
        '-f', '/proc/boot/bin/secpolpush', '-f', '/proc/boot/bin/secpolmonitor')
}
& $dumpifs @verifyArgs $imageInWork
if ($LASTEXITCODE -ne 0) { throw "Could not inspect the built IFS (exit $LASTEXITCODE)" }
$configPath = Join-Path $verifyDir 'proc\boot\pci_server.cfg'
$startupCheckPath = Join-Path $verifyDir 'proc\boot\startup.sh'
$config = Get-Content -LiteralPath $configPath -Raw
$startupCheck = Get-Content -LiteralPath $startupCheckPath -Raw
if ($BaselinePciServer) {
    $requiredConfig = @('DO_BUS_CONFIG=no')
} elseif ($EventHandlerOnly) {
    $requiredConfig = @('PCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so', 'DO_BUS_CONFIG=no')
} else {
    $requiredConfig = @('PCI_SERVER_MODULE_LIST=/lib/dll/pci/pci_server-event_handler.so',
        'DO_BUS_CONFIG=yes', "PCI_SERVER_BUSCFG_MODULE=$buscfgModule")
}
foreach ($required in $requiredConfig) {
    if ($config -notmatch [regex]::Escape($required)) {
        throw "Built IFS config is missing: $required"
    }
}
if (-not $BaselinePciServer -and -not $SupervisorMode -and -not $PciOnly) {
    foreach ($required in @('/proc/boot/quiver-pci-rm &')) {
        if (-not $startupCheck.Contains($required)) {
            throw "Built IFS startup is missing: $required"
        }
    }
}
if ($ActivePolicySource) {
    if (-not $startupCheck.Contains('secpolpush')) {
        throw 'Built IFS startup is missing secpolpush'
    }
    if (-not $startupCheck.Contains("on -T $PciServerType pci-server")) {
        throw 'Built IFS does not launch pci-server under its requested policy type'
    }
    if (-not $SupervisorMode -and -not $startupCheck.Contains("on -T $QuiverManagerType /proc/boot/quiver-pci-rm")) {
        throw 'Built IFS does not launch the Quiver manager under its requested policy type'
    }
    if ($SupervisorMode -and -not $startupCheck.Contains("on -T quiver_supervisor_t /proc/boot/quiver-supervisor")) {
        throw 'Built IFS does not launch the Quiver supervisor under quiver_supervisor_t'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $verifyDir 'proc\boot\secpol.bin'))) {
        throw 'Built IFS is missing /proc/boot/secpol.bin'
    }
    foreach ($policyTool in @('secpolpush', 'secpolmonitor')) {
        if (-not (Test-Path -LiteralPath (Join-Path $verifyDir ("proc\boot\bin\$policyTool")))) {
            throw "Built IFS is missing /proc/boot/bin/$policyTool"
        }
    }
}

$outputParent = Split-Path $OutputIfs -Parent
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
Copy-Item -LiteralPath $imageInWork -Destination $OutputIfs -Force
Write-Output "PASS: built and inspected software-hotplug test IFS: $OutputIfs"
Write-Output "Work files: $workRoot"
