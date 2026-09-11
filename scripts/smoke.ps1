param([string]$BuildDirectory = "$PSScriptRoot\..\build\Release")
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class PControlWindowTest {
    public delegate bool Visitor(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(Visitor visitor, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, Visitor visitor, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr window, uint msg, IntPtr wparam, StringBuilder text, uint flags, uint timeout, out IntPtr result);
    public static bool HasStatus(uint pid, string expected) {
        bool found = false;
        EnumWindows((window, data) => { uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == pid) EnumChildWindows(window, (child, unused) => {
                var text = new StringBuilder(2048); IntPtr result;
                if (SendMessageTimeout(child, 13, (IntPtr)2048, text, 2, 1000, out result) != IntPtr.Zero)
                    found |= text.ToString().StartsWith(expected);
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static bool Exists(uint pid) {
        bool found = false;
        EnumWindows((window, data) => { uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == pid) { var text = new StringBuilder(256); GetWindowText(window, text, 256); found |= text.ToString() == "PControl"; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$binary = (Resolve-Path -LiteralPath $BuildDirectory).Path
$corePath = Join-Path $binary 'PControl.Core.exe'
$guiPath = Join-Path $binary 'PControl.exe'
$targetPath = Join-Path $binary 'PControl.TestTarget.exe'
if (Get-Process -Name 'PControl.Core','PControl' -ErrorAction SilentlyContinue) { throw 'Close existing PControl/Core before this isolated smoke test.' }
$testRoot = Join-Path $env:TEMP ('PControl.Smoke.' + [guid]::NewGuid())
$previousData = $env:PCONTROL_DATA_DIR
$env:PCONTROL_DATA_DIR = $testRoot
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$session = (Get-Process -Id $PID).SessionId
$pipeName = "PControl.v1.$sid.$session"
$script:checks = 0
$owned = [Collections.Generic.List[Diagnostics.Process]]::new()
function Assert-Test($condition, [string]$name) {
    if (-not $condition) { throw "FAILED: $name" }
    $script:checks++
    Write-Output "PASS: $name"
}
function Read-Exact($stream, [int]$length) {
    $buffer = [byte[]]::new($length)
    $offset = 0
    while ($offset -lt $length) {
        $task = $stream.ReadAsync($buffer, $offset, $length - $offset)
        if (-not $task.Wait(6000)) { throw 'IPC read timeout' }
        if ($task.Result -eq 0) { throw 'IPC disconnected' }
        $offset += $task.Result
    }
    return ,$buffer
}
function Invoke-Core([hashtable]$message, [string]$Raw = '') {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect(3000)
        if (-not $message.ContainsKey('version')) { $message.version = 1 }
        $body = if ($Raw) { $Raw } else { $message | ConvertTo-Json -Depth 12 -Compress }
        $bytes = [Text.Encoding]::UTF8.GetBytes($body)
        $header = [BitConverter]::GetBytes([uint32]$bytes.Length)
        $pipe.Write($header, 0, 4)
        $pipe.Write($bytes, 0, $bytes.Length)
        $size = [BitConverter]::ToUInt32((Read-Exact $pipe 4), 0)
        if ($size -gt 4194304) { throw 'Oversized response' }
        $reply = [Text.Encoding]::UTF8.GetString((Read-Exact $pipe $size)) | ConvertFrom-Json
        $pipe.WriteByte(1)
        return $reply
    } finally { $pipe.Dispose() }
}
function Start-Owned([string]$path) {
    $process = Start-Process -FilePath $path -WindowStyle Hidden -PassThru
    $owned.Add($process)
    return $process
}
function Wait-Core {
    for ($attempt = 0; $attempt -lt 25; $attempt++) {
        try { $r = Invoke-Core @{command='Ping'}; if ($r.success) { return $r } } catch {}
        Start-Sleep -Milliseconds 100
    }
    throw 'Core not ready'
}
function Find-Target([int]$targetId) {
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        $r = Invoke-Core @{command='GetProcessList'}
        $row = $r.data | Where-Object pid -eq $targetId
        if ($row) { return $row }
        Start-Sleep -Milliseconds 150
    }
    throw 'Target not detected'
}
try {
    $gui = Start-Owned $guiPath
    $initial = Wait-Core
    $coreId = $initial.data.pid
    Assert-Test ($coreId -gt 0) 'GUI auto-starts separate Core'
    Start-Sleep -Milliseconds 700
    $gui.Refresh()
    Assert-Test (-not $gui.HasExited -and [PControlWindowTest]::Exists($gui.Id)) 'GUI window created'
    Assert-Test ([PControlWindowTest]::HasStatus($gui.Id, 'Core: Running')) 'GUI shows Running status and answers window messages'
    $duplicate = Start-Owned $corePath
    Assert-Test ($duplicate.WaitForExit(5000) -and $duplicate.ExitCode -eq 0) 'Second Core exits cleanly'
    Stop-Process -Id $gui.Id
    Assert-Test ((Invoke-Core @{command='Ping'}).data.pid -eq $coreId) 'Core survives GUI termination'
    $gui = Start-Owned $guiPath
    Start-Sleep -Milliseconds 700
    Assert-Test ((Invoke-Core @{command='Ping'}).data.pid -eq $coreId) 'GUI restart reuses Core'
    Assert-Test (-not (Invoke-Core @{command='Ping';version=999}).success) 'Protocol mismatch rejected'
    Assert-Test (-not (Invoke-Core @{} -Raw '{broken').success) 'Malformed JSON rejected with reply'
    Assert-Test (-not (Invoke-Core @{command='Unknown'}).success) 'Unknown request rejected'
    $idle = [IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [IO.Pipes.PipeDirection]::InOut)
    $idle.Connect(3000)
    Start-Sleep -Milliseconds 3400
    $idle.Dispose()
    Assert-Test ((Invoke-Core @{command='Ping'}).success) 'Idle client times out; server recovers'
    for ($i=0; $i -lt 20; $i++) { $null = Invoke-Core @{command='Ping'} }
    $beforeHandles = (Get-Process -Id $coreId).HandleCount
    for ($i=0; $i -lt 100; $i++) { $null = Invoke-Core @{command='Ping'} }
    $afterHandles = (Get-Process -Id $coreId).HandleCount
    Assert-Test ($afterHandles -le $beforeHandles + 8) "No linear handle growth across 100 requests ($beforeHandles -> $afterHandles)"
    $target = Start-Owned $targetPath
    $row = Find-Target $target.Id
    Assert-Test ($row.creationTime -ne '0') 'New process has creation identity'
    $base = @{pid=$target.Id;creationTime=$row.creationTime}
    Assert-Test ((Invoke-Core ($base + @{command='GetProcessDetails'})).success) 'Current process details IPC'
    Assert-Test ((Invoke-Core ($base + @{command='SetPriority';value=0x4000})).success) 'Current priority IPC'
    $target.Refresh()
    Assert-Test ($target.PriorityClass -eq 'BelowNormal') 'Windows confirms priority'
    Assert-Test (-not (Invoke-Core @{command='SetPriority';pid=$target.Id;creationTime='1';value=0x20}).success) 'Stale creation time rejected'
    $multiGroup = $initial.data.processorGroups -gt 1
    if (-not $multiGroup) {
        $available = [uint64]$row.systemMask
        Assert-Test ($available -ne 0) 'System affinity available'
        $cpu = 0
        while (($available -band ([uint64]1 -shl $cpu)) -eq 0) { $cpu++ }
        $affinity = @{group=0;cpus=@($cpu)}
        Assert-Test ((Invoke-Core ($base + @{command='SetAffinity';value=$affinity})).success) 'Current affinity IPC'
        $target.Refresh()
        Assert-Test ($target.ProcessorAffinity.ToInt64() -eq ([int64]1 -shl $cpu)) 'Windows confirms affinity'
        Assert-Test (-not (Invoke-Core ($base + @{command='SetAffinity';value=@{group=0;cpus=@()}})).success) 'Zero CPU selection rejected'
    }
    Assert-Test ((Invoke-Core @{command='AddPersistentPriorityRule';name='pcontrol.testtarget.EXE';value=0x40}).success) 'Save priority rule case-insensitively'
    $target.Refresh()
    Assert-Test ($target.PriorityClass -eq 'Idle') 'Rule applies immediately'
    if (-not $multiGroup) { Assert-Test ((Invoke-Core @{command='AddPersistentAffinityRule';name='PControl.TestTarget.exe';value=$affinity}).success) 'Save affinity rule' }
    Stop-Process -Id $gui.Id
    Stop-Process -Id $target.Id
    Assert-Test (-not (Invoke-Core ($base + @{command='SetPriority';value=0x20})).success) 'Process-exit race returns failure'
    Start-Sleep -Milliseconds 1800
    Assert-Test (-not ((Invoke-Core @{command='GetProcessList'}).data | Where-Object pid -eq $target.Id)) 'Terminated process removed'
    $target = Start-Owned $targetPath
    $null = Find-Target $target.Id
    $target.Refresh()
    Assert-Test ($target.PriorityClass -eq 'Idle') 'New instance receives rule without GUI'
    if (-not $multiGroup) { Assert-Test ($target.ProcessorAffinity.ToInt64() -eq ([int64]1 -shl $cpu)) 'New instance receives affinity without GUI' }
    Stop-Process -Id $target.Id
    $null = Invoke-Core @{command='Shutdown'}
    Start-Sleep -Milliseconds 600
    $core = Start-Owned $corePath
    $null = Wait-Core
    $target = Start-Owned $targetPath
    $null = Find-Target $target.Id
    $target.Refresh()
    Assert-Test ($target.PriorityClass -eq 'Idle') 'Persistent priority survives Core restart'
    if (-not $multiGroup) { Assert-Test ($target.ProcessorAffinity.ToInt64() -eq ([int64]1 -shl $cpu)) 'Persistent affinity survives Core restart' }
    Assert-Test ((Invoke-Core @{command='SetUpdateInterval';value=500}).success) 'Configurable monitor interval'
    Assert-Test ((Invoke-Core @{command='GetStatus'}).data.intervalMs -eq 500) 'Updated interval exposed'
    Assert-Test ((Invoke-Core @{command='ReloadConfiguration'}).success) 'Config reload'
    Assert-Test ((Invoke-Core @{command='RemovePersistentPriorityRule';name='PControl.TestTarget.exe'}).success) 'Remove priority rule'
    if (-not $multiGroup) { Assert-Test ((Invoke-Core @{command='RemovePersistentAffinityRule';name='PControl.TestTarget.exe'}).success) 'Remove affinity rule' }
    $gui = Start-Owned $guiPath
    Start-Sleep -Milliseconds 700
    $null = Invoke-Core @{command='Shutdown'}
    Start-Sleep -Milliseconds 2200
    $gui.Refresh()
    Assert-Test (-not $gui.HasExited) 'GUI survives Core disconnect'
    Assert-Test ([PControlWindowTest]::HasStatus($gui.Id, 'Core: Disconnected')) 'GUI displays Disconnected and remains responsive'
    Assert-Test ((Get-Content -LiteralPath (Join-Path $testRoot 'Core.log') -Raw) -match 'Core shutdown') 'Lifecycle logging persisted'
    Write-Output "Completed $script:checks runtime checks. Evidence: $testRoot"
} finally {
    foreach ($process in $owned) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue } }
    try { $null = Invoke-Core @{command='Shutdown'} } catch {}
    $env:PCONTROL_DATA_DIR = $previousData
}
