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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint allocation, uint protection);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr written);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Sequential)] struct LVITEM {
        public uint mask; public int iItem; public int iSubItem; public uint state; public uint stateMask;
        public IntPtr pszText; public int cchTextMax; public int iImage; public IntPtr lParam; public int iIndent;
        public int iGroupId; public uint cColumns; public IntPtr puColumns; public IntPtr piColFmt; public int iGroup;
    }
    static IntPtr ProcessList(uint pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, data) => { uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == pid) EnumChildWindows(window, (child, unused) => {
                var name = new StringBuilder(64); GetClassName(child, name, 64);
                if (name.ToString() == "SysListView32" && (GetWindowLong(child, -16) & 3) == 1) result = child;
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        if (result == IntPtr.Zero) throw new InvalidOperationException("Process list view not found");
        return result;
    }
    public static int ItemCount(uint pid) { return SendMessage(ProcessList(pid), 0x1004, IntPtr.Zero, IntPtr.Zero).ToInt32(); }
    public static int TopIndex(uint pid) { return SendMessage(ProcessList(pid), 0x1027, IntPtr.Zero, IntPtr.Zero).ToInt32(); }
    public static int SelectedIndex(uint pid) { return SendMessage(ProcessList(pid), 0x100c, new IntPtr(-1), new IntPtr(2)).ToInt32(); }
    public static int ScrollTo(uint pid, int index) {
        SendMessage(ProcessList(pid), 0x1013, new IntPtr(index), IntPtr.Zero);
        return TopIndex(pid);
    }
    public static void Select(uint pid, int index) {
        var process = OpenProcess(0x0008 | 0x0020, false, pid);
        if (process == IntPtr.Zero) throw new InvalidOperationException("OpenProcess for list selection failed");
        int size = Marshal.SizeOf(typeof(LVITEM));
        var remote = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)size, 0x1000 | 0x2000, 0x04);
        try {
            var item = new LVITEM { state = 3, stateMask = 3 };
            var local = Marshal.AllocHGlobal(size);
            try {
                Marshal.StructureToPtr(item, local, false); var bytes = new byte[size]; Marshal.Copy(local, bytes, 0, size);
                IntPtr written; if (!WriteProcessMemory(process, remote, bytes, size, out written) || written.ToInt64() != size)
                    throw new InvalidOperationException("WriteProcessMemory for list selection failed");
            } finally { Marshal.FreeHGlobal(local); }
            SendMessage(ProcessList(pid), 0x102b, new IntPtr(index), remote);
        } finally { if (remote != IntPtr.Zero) VirtualFreeEx(process, remote, UIntPtr.Zero, 0x8000); CloseHandle(process); }
    }
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
    Start-Sleep -Milliseconds 1800
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
    Start-Sleep -Milliseconds 1800
    $beforeRows = @((Invoke-Core @{command='GetProcessList'}).data)
    Assert-Test ($beforeRows.Count -eq [PControlWindowTest]::ItemCount($gui.Id)) 'GUI list reflects sorted Core snapshot'
    $desiredTop = [Math]::Max(1, [Math]::Floor($beforeRows.Count / 2))
    $oldTop = [PControlWindowTest]::ScrollTo($gui.Id, $desiredTop)
    $topIdentity = $beforeRows[$oldTop]
    $oldSelected = [Math]::Min($oldTop + 2, $beforeRows.Count - 1)
    $selectedIdentity = $beforeRows[$oldSelected]
    [PControlWindowTest]::Select($gui.Id, $oldSelected)
    Assert-Test ([PControlWindowTest]::SelectedIndex($gui.Id) -eq $oldSelected) 'Runtime test selects a process low in the list'
    $scrollTargetPath = Join-Path $testRoot '000-PControl-Scroll.exe'
    Copy-Item -LiteralPath $targetPath -Destination $scrollTargetPath
    $scrollTarget = Start-Owned $scrollTargetPath
    $null = Find-Target $scrollTarget.Id
    Start-Sleep -Milliseconds 2200
    $afterRows = @((Invoke-Core @{command='GetProcessList'}).data)
    $newTop = 0
    while ($newTop -lt $afterRows.Count -and
           ($afterRows[$newTop].pid -ne $topIdentity.pid -or $afterRows[$newTop].creationTime -ne $topIdentity.creationTime)) { $newTop++ }
    $newSelected = 0
    while ($newSelected -lt $afterRows.Count -and
           ($afterRows[$newSelected].pid -ne $selectedIdentity.pid -or $afterRows[$newSelected].creationTime -ne $selectedIdentity.creationTime)) { $newSelected++ }
    Assert-Test ($newTop -lt $afterRows.Count -and [PControlWindowTest]::TopIndex($gui.Id) -eq $newTop) 'Top-visible process identity survives refresh and insertion'
    Assert-Test ($newSelected -lt $afterRows.Count -and [PControlWindowTest]::SelectedIndex($gui.Id) -eq $newSelected) 'Selected process identity survives refresh and insertion'
    Stop-Process -Id $scrollTarget.Id
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
