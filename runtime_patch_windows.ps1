param(
    [int]$TimeoutSeconds = 120,
    [string]$ExeName = "battlezone98redux.exe",
    [int]$TargetPid = 0
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

function Find-PatternOffsets {
    param(
        [byte[]]$Buffer,
        [byte[]]$Pattern
    )
    $hits = New-Object System.Collections.Generic.List[int]
    $max = $Buffer.Length - $Pattern.Length
    if ($max -lt 0) {
        return $hits
    }
    for ($i = 0; $i -le $max; $i++) {
        $ok = $true
        for ($j = 0; $j -lt $Pattern.Length; $j++) {
            if ($Buffer[$i + $j] -ne $Pattern[$j]) {
                $ok = $false
                break
            }
        }
        if ($ok) {
            [void]$hits.Add($i)
        }
    }
    return $hits
}

function Choose-AddressPair {
    param(
        [int[]]$SendImms,
        [int[]]$RecvImms,
        [int]$PreferredDelta = 0x1F4
    )

    $pairs = New-Object System.Collections.Generic.List[object]
    $recvSet = New-Object 'System.Collections.Generic.HashSet[int]'
    foreach ($r in $RecvImms) { [void]$recvSet.Add($r) }

    foreach ($s in $SendImms) {
        $r = $s + $PreferredDelta
        if ($recvSet.Contains($r)) {
            [void]$pairs.Add(@($s, $r, 0))
        }
    }

    if ($pairs.Count -eq 0) {
        foreach ($s in $SendImms) {
            foreach ($r in $RecvImms) {
                $d = $r - $s
                if ($d -gt 0 -and $d -le 0x2000) {
                    [void]$pairs.Add(@($s, $r, [Math]::Abs($d - $PreferredDelta)))
                }
            }
        }
    }

    if ($pairs.Count -eq 0) {
        return @("none")
    }

    $sorted = $pairs | Sort-Object { [int]$_[2] }
    $bestScore = [int]$sorted[0][2]
    $best = @($sorted | Where-Object { [int]$_[2] -eq $bestScore })

    if ($best.Count -eq 1) {
        return @("ok", [int]$best[0][0], [int]$best[0][1], $bestScore)
    }

    return @("ambiguous", $best, $bestScore)
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
    $sendSigOld = [byte[]](0x68,0x00,0x80,0x00,0x00,0x6A,0x00,0x53,0xFF,0x95)
    $sendSigNew = [byte[]](0x68,0x00,0x00,0x08,0x00,0x6A,0x00,0x53,0xFF,0x95)
    $recvSigOld = [byte[]](0x68,0x00,0x00,0x10,0x00,0xFF,0x95,0xD8,0xFE,0xFF,0xFF)
    $recvSigNew = [byte[]](0x68,0x00,0x00,0x20,0x00,0xFF,0x95,0xD8,0xFE,0xFF,0xFF)
    $sendHitSet = New-Object 'System.Collections.Generic.HashSet[int]'
    $recvHitSet = New-Object 'System.Collections.Generic.HashSet[int]'

    # Stage 1: strong signatures (instruction context + immediate).
    foreach ($sig in @($sendSigOld, $sendSigNew)) {
        $hits = Find-PatternOffsets -Buffer $moduleBytes -Pattern $sig
        foreach ($h in $hits) { [void]$sendHitSet.Add($h + 1) }
    }
    foreach ($sig in @($recvSigOld, $recvSigNew)) {
        $hits = Find-PatternOffsets -Buffer $moduleBytes -Pattern $sig
        foreach ($h in $hits) { [void]$recvHitSet.Add($h + 1) }
    }

    # Stage 2: loose dword candidates if signatures drift across builds.
    foreach ($needle in $sendNeedles) {
        $hits = Find-DwordOffsets -Buffer $moduleBytes -Value4 $needle
        foreach ($h in $hits) {
            if ($h -gt 0 -and $moduleBytes[$h - 1] -eq 0x68) {
                [void]$sendHitSet.Add($h)
            }
        }
    }
    foreach ($needle in $recvNeedles) {
        $hits = Find-DwordOffsets -Buffer $moduleBytes -Value4 $needle
        foreach ($h in $hits) {
            if ($h -gt 0 -and $moduleBytes[$h - 1] -eq 0x68) {
                [void]$recvHitSet.Add($h)
            }
        }
    }

    $sendImms = @($sendHitSet)
    $recvImms = @($recvHitSet)
    $picked = Choose-AddressPair -SendImms $sendImms -RecvImms $recvImms -PreferredDelta 0x1F4

    if ($picked[0] -eq "ok") {
        $sendAddr = [IntPtr]($Base + [int]$picked[1])
        $recvAddr = [IntPtr]($Base + [int]$picked[2])
        $score = [int]$picked[3]
        return @($sendAddr, $recvAddr, ("auto(score={0})" -f $score))
    }

    if ($picked[0] -eq "ambiguous") {
        Write-Host "Found multiple candidate address pairs; cannot choose safely."
        $list = $picked[1]
        foreach ($p in $list | Select-Object -First 8) {
            $sva = $Base + [int]$p[0]
            $rva = $Base + [int]$p[1]
            $scr = [int]$p[2]
            Write-Host ("- send 0x{0:X8}, recv 0x{1:X8}, score {2}" -f $sva, $rva, $scr)
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
if ($TargetPid -gt 0) {
    try {
        $target = Get-Process -Id $TargetPid -ErrorAction Stop
    }
    catch {
        Write-Host "Provided PID not found: $TargetPid"
        exit 2
    }
} else {
    while ((Get-Date) -lt $deadline) {
        $target = Get-TargetProcess -TargetExe $ExeName
        if ($null -ne $target) { break }
        Start-Sleep -Seconds 1
    }
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
