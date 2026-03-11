param(
    [int]$TimeoutSeconds = 120,
    [string]$ExeName = "battlezone98redux.exe"
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NativeMethods {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(UInt32 dwDesiredAccess, bool bInheritHandle, int dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, Int32 nSize, out IntPtr lpNumberOfBytesRead);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, Int32 nSize, out IntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool VirtualProtectEx(IntPtr hProcess, IntPtr lpAddress, UIntPtr dwSize, UInt32 flNewProtect, out UInt32 lpflOldProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);
}
"@

$PROCESS_VM_READ = 0x0010
$PROCESS_VM_WRITE = 0x0020
$PROCESS_VM_OPERATION = 0x0008
$PROCESS_QUERY_INFORMATION = 0x0400
$PAGE_EXECUTE_READWRITE = 0x40

function Get-TargetProcess {
    param([string]$TargetExe)
    $needle = $TargetExe.ToLowerInvariant()
    $procs = Get-Process -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        try {
            if ($p.Path -and ([System.IO.Path]::GetFileName($p.Path).ToLowerInvariant() -eq $needle)) {
                return $p
            }
        }
        catch {
            continue
        }
    }
    return $null
}

function Read-Memory4 {
    param(
        [IntPtr]$Handle,
        [IntPtr]$Address
    )
    $buf = New-Object byte[] 4
    $read = [IntPtr]::Zero
    $ok = [NativeMethods]::ReadProcessMemory($Handle, $Address, $buf, 4, [ref]$read)
    if (-not $ok) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "ReadProcessMemory failed at 0x$('{0:X}' -f $Address.ToInt64()) (Win32 $err)"
    }
    return $buf
}

function Read-Memory {
    param(
        [IntPtr]$Handle,
        [IntPtr]$Address,
        [int]$Size
    )
    $buf = New-Object byte[] $Size
    $read = [IntPtr]::Zero
    $ok = [NativeMethods]::ReadProcessMemory($Handle, $Address, $buf, $Size, [ref]$read)
    if (-not $ok -or $read.ToInt32() -ne $Size) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "ReadProcessMemory failed at 0x$('{0:X}' -f $Address.ToInt64()) size $Size (Win32 $err)"
    }
    return $buf
}

function Find-DwordOffsets {
    param(
        [byte[]]$Buffer,
        [byte[]]$Value4
    )
    $hits = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -le ($Buffer.Length - 4); $i++) {
        if ($Buffer[$i] -eq $Value4[0] -and
            $Buffer[$i + 1] -eq $Value4[1] -and
            $Buffer[$i + 2] -eq $Value4[2] -and
            $Buffer[$i + 3] -eq $Value4[3]) {
            [void]$hits.Add($i)
        }
    }
    return $hits
}

function Resolve-PatchAddresses {
    param(
        [IntPtr]$Handle,
        [Int64]$Base,
        [int]$ModuleSize,
        [IntPtr]$FixedSendAddr,
        [IntPtr]$FixedRecvAddr,
        [byte[]]$SendOld,
        [byte[]]$SendNew,
        [byte[]]$RecvOld,
        [byte[]]$RecvNew
    )

    # Fast path: fixed offsets still match known bytes.
    $sendBefore = Read-Memory4 -Handle $Handle -Address $FixedSendAddr
    $recvBefore = Read-Memory4 -Handle $Handle -Address $FixedRecvAddr
    $sendHexBefore = ($sendBefore | ForEach-Object { $_.ToString("x2") }) -join ""
    $recvHexBefore = ($recvBefore | ForEach-Object { $_.ToString("x2") }) -join ""
    if (($sendHexBefore -in @("00800000", "00000800")) -and ($recvHexBefore -in @("00001000", "00002000"))) {
        return @($FixedSendAddr, $FixedRecvAddr, "fixed")
    }

    Write-Host "Fixed offsets did not match known bytes. Attempting automatic pattern discovery..."

    $moduleBytes = Read-Memory -Handle $Handle -Address ([IntPtr]$Base) -Size $ModuleSize

    $sendNeedles = @($SendOld, $SendNew)
    $recvNeedles = @($RecvOld, $RecvNew)
    $sendHitSet = New-Object 'System.Collections.Generic.HashSet[int]'
    $recvHitSet = New-Object 'System.Collections.Generic.HashSet[int]'

    foreach ($needle in $sendNeedles) {
        $hits = Find-DwordOffsets -Buffer $moduleBytes -Value4 $needle
        foreach ($h in $hits) { [void]$sendHitSet.Add($h) }
    }
    foreach ($needle in $recvNeedles) {
        $hits = Find-DwordOffsets -Buffer $moduleBytes -Value4 $needle
        foreach ($h in $hits) { [void]$recvHitSet.Add($h) }
    }

    $delta = 0x1F4
    $candidatePairs = New-Object System.Collections.Generic.List[object]
    foreach ($s in $sendHitSet) {
        $r = $s + $delta
        if ($recvHitSet.Contains($r)) {
            [void]$candidatePairs.Add(@($s, $r))
        }
    }

    if ($candidatePairs.Count -eq 1) {
        $pair = $candidatePairs[0]
        $sendAddr = [IntPtr]($Base + [int]$pair[0])
        $recvAddr = [IntPtr]($Base + [int]$pair[1])
        return @($sendAddr, $recvAddr, "auto")
    }

    if ($candidatePairs.Count -gt 1) {
        Write-Host "Found multiple candidate address pairs; cannot choose safely."
        foreach ($p in $candidatePairs) {
            $sva = $Base + [int]$p[0]
            $rva = $Base + [int]$p[1]
            Write-Host ("- send 0x{0:X8}, recv 0x{1:X8}" -f $sva, $rva)
        }
        throw "Ambiguous runtime patch addresses"
    }

    throw "Could not resolve runtime patch addresses for this build"
}

function Write-Memory4 {
    param(
        [IntPtr]$Handle,
        [IntPtr]$Address,
        [byte[]]$Data
    )
    $oldProt = 0
    $okProt = [NativeMethods]::VirtualProtectEx($Handle, $Address, [UIntPtr]4, $PAGE_EXECUTE_READWRITE, [ref]$oldProt)
    if (-not $okProt) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "VirtualProtectEx failed at 0x$('{0:X}' -f $Address.ToInt64()) (Win32 $err)"
    }

    try {
        $written = [IntPtr]::Zero
        $okWrite = [NativeMethods]::WriteProcessMemory($Handle, $Address, $Data, 4, [ref]$written)
        if (-not $okWrite -or $written.ToInt32() -ne 4) {
            $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "WriteProcessMemory failed at 0x$('{0:X}' -f $Address.ToInt64()) (Win32 $err)"
        }
    }
    finally {
        $tmp = 0
        [void][NativeMethods]::VirtualProtectEx($Handle, $Address, [UIntPtr]4, [UInt32]$oldProt, [ref]$tmp)
    }
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
Write-Host "Waiting for running process: $ExeName"
$target = $null
while ((Get-Date) -lt $deadline) {
    $target = Get-TargetProcess -TargetExe $ExeName
    if ($null -ne $target) { break }
    Start-Sleep -Seconds 1
}

if ($null -eq $target) {
    Write-Host "Timed out after $TimeoutSeconds s waiting for $ExeName"
    Write-Host "Start the game first, wait at in-game main menu, then rerun this script."
    exit 2
}

Write-Host "Found process PID: $($target.Id)"

$base = [Int64]$target.MainModule.BaseAddress
$moduleSize = [int]$target.MainModule.ModuleMemorySize
$fixedSendAddr = [IntPtr]($base + 0x12D96A)
$fixedRecvAddr = [IntPtr]($base + 0x12DB5E)

$sendOld = [byte[]](0x00,0x80,0x00,0x00)
$sendNew = [byte[]](0x00,0x00,0x08,0x00)
$recvOld = [byte[]](0x00,0x00,0x10,0x00)
$recvNew = [byte[]](0x00,0x00,0x20,0x00)

$access = $PROCESS_QUERY_INFORMATION -bor $PROCESS_VM_READ -bor $PROCESS_VM_WRITE -bor $PROCESS_VM_OPERATION
$hProc = [NativeMethods]::OpenProcess($access, $false, $target.Id)
if ($hProc -eq [IntPtr]::Zero) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "OpenProcess failed (Win32 $err). Try running PowerShell as Administrator."
    exit 3
}

try {
    $resolved = Resolve-PatchAddresses -Handle $hProc -Base $base -ModuleSize $moduleSize -FixedSendAddr $fixedSendAddr -FixedRecvAddr $fixedRecvAddr -SendOld $sendOld -SendNew $sendNew -RecvOld $recvOld -RecvNew $recvNew
    $sendAddr = [IntPtr]$resolved[0]
    $recvAddr = [IntPtr]$resolved[1]
    $mode = [string]$resolved[2]

    Write-Host ("Using {0} addresses:" -f $mode)
    Write-Host ("- send: 0x{0:X8}" -f $sendAddr.ToInt64())
    Write-Host ("- recv: 0x{0:X8}" -f $recvAddr.ToInt64())

    $sendBefore = Read-Memory4 -Handle $hProc -Address $sendAddr
    $recvBefore = Read-Memory4 -Handle $hProc -Address $recvAddr

    $sendHexBefore = ($sendBefore | ForEach-Object { $_.ToString("x2") }) -join ""
    $recvHexBefore = ($recvBefore | ForEach-Object { $_.ToString("x2") }) -join ""

    if ((-not ($sendHexBefore -in @("00800000", "00000800"))) -or (-not ($recvHexBefore -in @("00001000", "00002000")))) {
        Write-Host "Unexpected bytes at runtime patch addresses."
        Write-Host "send before: $sendHexBefore"
        Write-Host "recv before: $recvHexBefore"
        exit 4
    }

    Write-Memory4 -Handle $hProc -Address $sendAddr -Data $sendNew
    Write-Memory4 -Handle $hProc -Address $recvAddr -Data $recvNew

    $sendAfter = Read-Memory4 -Handle $hProc -Address $sendAddr
    $recvAfter = Read-Memory4 -Handle $hProc -Address $recvAddr

    $sendHexAfter = ($sendAfter | ForEach-Object { $_.ToString("x2") }) -join ""
    $recvHexAfter = ($recvAfter | ForEach-Object { $_.ToString("x2") }) -join ""

    Write-Host "send now: $sendHexAfter"
    Write-Host "recv now: $recvHexAfter"

    if ($sendHexAfter -eq "00000800" -and $recvHexAfter -eq "00002000") {
        Write-Host "Runtime patch applied in memory."
        exit 0
    }

    Write-Host "Runtime patch could not be verified."
    exit 5
}
finally {
    [void][NativeMethods]::CloseHandle($hProc)
}
