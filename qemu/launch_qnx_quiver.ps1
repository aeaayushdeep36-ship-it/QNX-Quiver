[CmdletBinding()]
param(
    [string]$QemuPath = 'C:\Program Files\qemu\qemu-system-x86_64.exe',
    [string]$VmRoot = "$env:USERPROFILE\qnxvm",
    [string]$SharedMemoryPath = "$env:TEMP\qnx_quiver_shm_v3.bin",
    [switch]$ProbeOnly,
    [switch]$UseTcg
)

$ErrorActionPreference = 'Stop'
$diskPath = Join-Path $VmRoot 'output\disk-qemu.vmdk'
$ifsPath = Join-Path $VmRoot 'output\ifs.bin'

foreach ($path in @($QemuPath, $diskPath, $ifsPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

$deviceList = (& $QemuPath -device help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "Could not query QEMU device list at $QemuPath"
}
if ($deviceList -notmatch '(?m)ivshmem-plain') {
    throw "This QEMU build does not provide ivshmem-plain. QNX-Quiver will not launch a guest without the required virtual PCI bridge. Use an ivshmem-capable QEMU x86_64 build and rerun this script."
}
if ($ProbeOnly) {
    Write-Output 'QNX-Quiver prerequisites found: QEMU exposes ivshmem-plain and the QNX VM image exists.'
    exit 0
}

if (Test-Path -LiteralPath $SharedMemoryPath -PathType Leaf) {
    $existingSize = (Get-Item -LiteralPath $SharedMemoryPath).Length
    if ($existingSize -ne 4MB) {
        throw "Shared-memory file must be exactly 4 MiB; found $existingSize bytes at $SharedMemoryPath"
    }
} else {
    $parent = Split-Path -Parent $SharedMemoryPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $stream = [System.IO.File]::Open($SharedMemoryPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    try { $stream.SetLength(4MB) } finally { $stream.Dispose() }
}

$qemuShmPath = $SharedMemoryPath -replace '\\', '/'
$serialPath = Join-Path $PSScriptRoot '..\build\qemu_serial.log'
$serialPath = [System.IO.Path]::GetFullPath($serialPath)
$accel = if ($UseTcg) { 'tcg' } else { 'whpx,kernel-irqchip=off' }
$arguments = @(
    '-smp', '2',
    '-cpu', 'max',
    '-m', '1G',
    '-snapshot',
    '-drive', "file=$diskPath,if=ide,id=drv0",
    '-kernel', $ifsPath,
    '-object', "memory-backend-file,size=4M,share=on,mem-path=$qemuShmPath,id=qnx_shm",
    '-device', 'ivshmem-plain,memdev=qnx_shm,id=quiver_ivshmem',
    '-netdev', 'user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22',
    '-device', 'virtio-net-pci,netdev=net0',
    '-device', 'virtio-rng-pci',
    '-serial', "file:$serialPath",
    '-monitor', 'tcp:127.0.0.1:4442,server=on,wait=off',
    '-display', 'none',
    '-accel', $accel
)
$quotedArguments = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
$argumentLine = $quotedArguments -join ' '
$qemu = Start-Process -FilePath $QemuPath -ArgumentList $argumentLine -PassThru -WindowStyle Hidden
Write-Output "Started QNX-Quiver QEMU process $($qemu.Id)"
Write-Output "Serial log: $serialPath"
Write-Output 'SSH forwarding: 127.0.0.1:2222 -> guest:22'
