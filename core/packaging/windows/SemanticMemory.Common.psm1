Set-StrictMode -Version Latest

function Get-SmSha256File {
    param([Parameter(Mandatory=$true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-SmSha256Text {
    param([Parameter(Mandatory=$true)][string]$Text)
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-SmSha256Bytes {
    param([Parameter(Mandatory=$true)][AllowEmptyCollection()][byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Write-SmUtf8NoBom {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Text
    )
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Write-SmBytesCreateNew {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][byte[]]$Bytes
    )
    $parent = Split-Path -Parent $Path
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw 'CONFIG_WRITE_PARENT_MISSING'
    }
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        4096,
        [System.IO.FileOptions]::WriteThrough
    )
    try {
        if ($Bytes.Length -gt 0) { $stream.Write($Bytes, 0, $Bytes.Length) }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Set-SmFileMetadataFromSnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Snapshot
    )
    $full = Assert-SmNoReparsePath -Path $Path
    if ($Snapshot.PSObject.Properties.Name -contains 'acl_sddl' -and
        -not [string]::IsNullOrWhiteSpace([string]$Snapshot.acl_sddl)) {
        $acl = Get-Acl -LiteralPath $full
        $acl.SetSecurityDescriptorSddlForm([string]$Snapshot.acl_sddl)
        Set-Acl -LiteralPath $full -AclObject $acl
    }
    if ($Snapshot.PSObject.Properties.Name -contains 'attributes' -and
        $null -ne $Snapshot.attributes) {
        [System.IO.File]::SetAttributes(
            $full,
            [System.IO.FileAttributes][int]$Snapshot.attributes
        )
    }
}

function Initialize-SmVerifiedDeleteNative {
    if ('SemanticMemory.Stage14.NativeVerifiedDelete' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace SemanticMemory.Stage14
{
    public static class NativeVerifiedDelete
    {
        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint Delete = 0x00010000;
        private const uint ReadControl = 0x00020000;
        private const uint FileReadAttributes = 0x00000080;
        private const uint FileWriteAttributes = 0x00000100;
        private const uint ShareRead = 0x00000001;
        private const uint ShareWrite = 0x00000002;
        private const uint ShareDelete = 0x00000004;
        private const uint OpenExisting = 3;
        private const uint CreateNew = 1;
        private const uint OpenReparsePoint = 0x00200000;
        private const uint BackupSemantics = 0x02000000;
        private const uint DeleteOnClose = 0x04000000;
        private const uint AttributeDirectory = 0x00000010;
        private const uint AttributeReparsePoint = 0x00000400;

        [StructLayout(LayoutKind.Sequential)]
        private struct FileDispositionInfo
        {
            [MarshalAs(UnmanagedType.Bool)]
            public bool DeleteFile;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ByHandleFileInformation
        {
            public uint FileAttributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
            public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct FileBasicInfo
        {
            public long CreationTime;
            public long LastAccessTime;
            public long LastWriteTime;
            public long ChangeTime;
            public uint FileAttributes;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle file,
            int fileInformationClass,
            ref FileDispositionInfo information,
            uint bufferSize);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandle(
            SafeFileHandle file,
            out ByHandleFileInformation information);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandleEx(
            SafeFileHandle file,
            int fileInformationClass,
            out FileBasicInfo information,
            uint bufferSize);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle file,
            int fileInformationClass,
            ref FileBasicInfo information,
            uint bufferSize);

        private static SafeFileHandle OpenWithAccess(string path, uint access)
        {
            SafeFileHandle handle = CreateFileW(
                path,
                access,
                ShareRead | ShareDelete,
                IntPtr.Zero,
                OpenExisting,
                OpenReparsePoint,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(error);
            }
            return handle;
        }

        public static SafeFileHandle OpenReadGuard(string path)
        {
            return OpenWithAccess(path, GenericRead | ReadControl);
        }

        public static SafeFileHandle OpenDirectoryLease(string path)
        {
            SafeFileHandle handle = CreateFileW(
                path,
                FileReadAttributes | ReadControl,
                ShareRead | ShareWrite,
                IntPtr.Zero,
                OpenExisting,
                BackupSemantics | OpenReparsePoint,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(error);
            }
            ByHandleFileInformation information;
            if (!GetFileInformationByHandle(handle, out information))
            {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(error);
            }
            if ((information.FileAttributes & AttributeDirectory) == 0 ||
                (information.FileAttributes & AttributeReparsePoint) != 0)
            {
                handle.Dispose();
                throw new InvalidOperationException(
                    "STABLE_DIRECTORY_LEASE_TARGET_INVALID");
            }
            return handle;
        }

        public static SafeFileHandle OpenDirectoryLeaseGuard(
            string path,
            SafeFileHandle directoryHandle)
        {
            string streamPath = path + ":semantic-memory-lease-" +
                Guid.NewGuid().ToString("N");
            SafeFileHandle guard = CreateFileW(
                streamPath,
                GenericRead | GenericWrite,
                ShareRead,
                IntPtr.Zero,
                CreateNew,
                BackupSemantics | DeleteOnClose,
                IntPtr.Zero);
            if (guard.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                guard.Dispose();
                throw new Win32Exception(error);
            }
            ByHandleFileInformation directoryInformation;
            ByHandleFileInformation guardInformation;
            if (!GetFileInformationByHandle(
                    directoryHandle,
                    out directoryInformation) ||
                !GetFileInformationByHandle(guard, out guardInformation))
            {
                int error = Marshal.GetLastWin32Error();
                guard.Dispose();
                throw new Win32Exception(error);
            }
            if (directoryInformation.VolumeSerialNumber !=
                    guardInformation.VolumeSerialNumber ||
                directoryInformation.FileIndexHigh != guardInformation.FileIndexHigh ||
                directoryInformation.FileIndexLow != guardInformation.FileIndexLow)
            {
                guard.Dispose();
                throw new InvalidOperationException(
                    "STABLE_DIRECTORY_LEASE_IDENTITY_CHANGED");
            }
            return guard;
        }

        public static SafeFileHandle OpenDelete(string path)
        {
            return OpenWithAccess(
                path,
                Delete | ReadControl | FileReadAttributes | FileWriteAttributes);
        }

        public static string Identity(SafeFileHandle handle)
        {
            ByHandleFileInformation information;
            if (!GetFileInformationByHandle(handle, out information))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            return information.VolumeSerialNumber.ToString("x8") + ":" +
                information.FileIndexHigh.ToString("x8") +
                information.FileIndexLow.ToString("x8");
        }

        public static void MarkDelete(SafeFileHandle handle)
        {
            FileDispositionInfo information = new FileDispositionInfo { DeleteFile = true };
            uint size = (uint)Marshal.SizeOf(typeof(FileDispositionInfo));
            if (!SetFileInformationByHandle(handle, 4, ref information, size))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }

        public static uint ClearReadOnly(SafeFileHandle handle)
        {
            FileBasicInfo information;
            uint size = (uint)Marshal.SizeOf(typeof(FileBasicInfo));
            if (!GetFileInformationByHandleEx(handle, 0, out information, size))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            uint original = information.FileAttributes;
            if ((original & 0x00000001) != 0)
            {
                information.FileAttributes = original & ~0x00000001U;
                if (information.FileAttributes == 0)
                {
                    information.FileAttributes = 0x00000080;
                }
                if (!SetFileInformationByHandle(handle, 0, ref information, size))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
            }
            return original;
        }

        public static void RestoreAttributes(SafeFileHandle handle, uint attributes)
        {
            FileBasicInfo information;
            uint size = (uint)Marshal.SizeOf(typeof(FileBasicInfo));
            if (!GetFileInformationByHandleEx(handle, 0, out information, size))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            information.FileAttributes = attributes;
            if (!SetFileInformationByHandle(handle, 0, ref information, size))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }
    }
}
'@
}

function Exit-SmStableDirectoryLease {
    param($Lease)
    if ($null -eq $Lease -or $null -eq $Lease.handles) { return }
    $handles = @($Lease.handles)
    for ($index = $handles.Count - 1; $index -ge 0; $index--) {
        if ($null -ne $handles[$index]) {
            $handles[$index].Dispose()
        }
    }
}

function Get-SmDirectoryIdentitySnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [string]$ErrorCode = 'STABLE_DIRECTORY_IDENTITY_FAILED'
    )
    Initialize-SmVerifiedDeleteNative
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $handle = $null
    try {
        $handle = [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenDirectoryLease(
            $full
        )
        return [SemanticMemory.Stage14.NativeVerifiedDelete]::Identity($handle)
    } catch {
        throw "${ErrorCode}: $($_.Exception.Message)"
    } finally {
        if ($null -ne $handle) {
            $handle.Dispose()
        }
    }
}

function Enter-SmStableDirectoryLease {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [switch]$RequireNewLeaf,
        [switch]$RequireExisting,
        [string]$ErrorCode = 'STABLE_DIRECTORY_LEASE_FAILED'
    )
    if ($RequireNewLeaf -and $RequireExisting) {
        throw "${ErrorCode}: incompatible lease requirements"
    }
    Initialize-SmVerifiedDeleteNative
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $root = [System.IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw "${ErrorCode}: path is not rooted"
    }
    $segments = @(
        $full.Substring($root.Length) -split '[\\/]+' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    if ($RequireNewLeaf -and $segments.Count -eq 0) {
        throw "${ErrorCode}: volume root cannot be a new leaf"
    }
    $handles = New-Object System.Collections.Generic.List[object]
    try {
        $handles.Add(
            [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenDirectoryLease(
                $root
            )
        )
        $cursor = $root
        for ($index = 0; $index -lt $segments.Count; $index++) {
            $cursor = Join-Path $cursor $segments[$index]
            $isLeaf = $index -eq ($segments.Count - 1)
            $exists = Test-Path -LiteralPath $cursor
            if ($RequireExisting -and -not $exists) {
                throw "${ErrorCode}: required directory is missing: $cursor"
            }
            if ($RequireNewLeaf -and $isLeaf -and $exists) {
                throw "${ErrorCode}: target already exists: $cursor"
            }
            if (-not $exists) {
                try {
                    New-Item -ItemType Directory -Path $cursor -ErrorAction Stop |
                        Out-Null
                } catch {
                    if ($RequireNewLeaf -and $isLeaf) { throw }
                    if (-not (Test-Path -LiteralPath $cursor -PathType Container)) {
                        throw
                    }
                }
            }
            $handles.Add(
                [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenDirectoryLease(
                    $cursor
                )
            )
        }
        if ($segments.Count -eq 0) {
            throw "${ErrorCode}: volume root cannot be leased"
        }
        $identity = [SemanticMemory.Stage14.NativeVerifiedDelete]::Identity(
            $handles[$handles.Count - 1]
        )
        $handles.Add(
            [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenDirectoryLeaseGuard(
                $full,
                $handles[$handles.Count - 1]
            )
        )
        return [pscustomobject]@{
            path = $full
            identity = $identity
            handles = [object[]]$handles.ToArray()
        }
    } catch {
        for ($index = $handles.Count - 1; $index -ge 0; $index--) {
            $handles[$index].Dispose()
        }
        throw "${ErrorCode}: $($_.Exception.Message)"
    }
}

function Enter-SmStableDirectoryAnchorLease {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [string]$ErrorCode = 'STABLE_DIRECTORY_ANCHOR_LEASE_FAILED',
        [string]$InvalidTargetErrorCode = ''
    )
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (Test-Path -LiteralPath $full) {
        if (-not (Test-Path -LiteralPath $full -PathType Container)) {
            throw "${ErrorCode}: target exists but is not a directory: $full"
        }
        $cursor = $full
    } else {
        $cursor = Split-Path -Parent $full
    }
    while ($cursor -and -not (Test-Path -LiteralPath $cursor)) {
        $parent = Split-Path -Parent $cursor
        if (-not $parent -or [string]::Equals(
            $parent,
            $cursor,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw "${ErrorCode}: no existing directory anchor: $full"
        }
        $cursor = $parent
    }
    if (-not $cursor -or -not (Test-Path -LiteralPath $cursor -PathType Container)) {
        throw "${ErrorCode}: existing anchor is not a directory: $cursor"
    }
    try {
        return Enter-SmStableDirectoryLease `
            -Path $cursor `
            -RequireExisting `
            -ErrorCode $ErrorCode
    } catch {
        if ($InvalidTargetErrorCode -and
            $_.Exception.Message -match 'STABLE_DIRECTORY_LEASE_TARGET_INVALID') {
            throw $InvalidTargetErrorCode
        }
        throw
    }
}

function Remove-SmOwnedSensitiveFile {
    param(
        [AllowEmptyString()][string]$Path,
        [AllowEmptyString()][string]$ExpectedSha256,
        $ExpectedSnapshot
    )
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $full = Assert-SmNoReparsePath -Path $Path
    Initialize-SmVerifiedDeleteNative
    $guardHandle = [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenReadGuard($full)
    try {
        # The guard denies write sharing while allowing a second delete handle.
        # Object identity binds the validated handle to the handle marked for
        # deletion, so a same-path replacement is detected rather than deleted.
        [void](Assert-SmNoReparsePath -Path $full)
        $observed = Get-SmConfigByteSnapshot -Path $full
        if ($null -ne $ExpectedSnapshot -and
            -not (Test-SmConfigSnapshotsEqual -Left $ExpectedSnapshot -Right $observed)) {
            throw 'CONFIG_SENSITIVE_TEMP_SNAPSHOT_MISMATCH'
        }
        if ($null -eq $ExpectedSnapshot -and
            -not [string]::IsNullOrWhiteSpace($ExpectedSha256) -and
            [string]$observed.sha256 -cne $ExpectedSha256) {
            throw 'CONFIG_SENSITIVE_TEMP_HASH_MISMATCH'
        }
        $deleteHandle = [SemanticMemory.Stage14.NativeVerifiedDelete]::OpenDelete($full)
        try {
            $guardIdentity = [SemanticMemory.Stage14.NativeVerifiedDelete]::Identity($guardHandle)
            $deleteIdentity = [SemanticMemory.Stage14.NativeVerifiedDelete]::Identity($deleteHandle)
            if ($guardIdentity -cne $deleteIdentity) {
                throw 'CONFIG_SENSITIVE_TEMP_IDENTITY_MISMATCH'
            }
            $originalAttributes = [SemanticMemory.Stage14.NativeVerifiedDelete]::ClearReadOnly(
                $deleteHandle
            )
            try {
                [SemanticMemory.Stage14.NativeVerifiedDelete]::MarkDelete($deleteHandle)
            } catch {
                [SemanticMemory.Stage14.NativeVerifiedDelete]::RestoreAttributes(
                    $deleteHandle,
                    $originalAttributes
                )
                throw
            }
        } finally {
            $deleteHandle.Dispose()
        }
    } finally {
        $guardHandle.Dispose()
    }
    if (Test-Path -LiteralPath $full) {
        throw 'CONFIG_SENSITIVE_TEMP_CLEANUP_FAILED'
    }
    return $true
}

function Assert-SmNoReparsePath {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [switch]$AllowMissingLeaf
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $AllowMissingLeaf -and -not (Test-Path -LiteralPath $full)) {
        throw 'CONFIG_PATH_MISSING'
    }
    $candidate = $full
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -Force -LiteralPath $candidate
            if ([bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw 'CONFIG_PATH_REPARSE_POINT'
            }
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or
            [string]::Equals($parent, $candidate, [System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $candidate = $parent
    }
    return $full
}

function Get-SmConfigByteSnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [switch]$AllowMissing
    )
    $full = Assert-SmNoReparsePath -Path $Path -AllowMissingLeaf
    $exists = Test-Path -LiteralPath $full -PathType Leaf
    if (-not $exists -and -not $AllowMissing) { throw 'CONFIG_PATH_MISSING' }
    if ((Test-Path -LiteralPath $full) -and -not $exists) { throw 'CONFIG_PATH_NOT_FILE' }

    [byte[]]$bytes = @()
    $text = ''
    $hasBom = $false
    if ($exists) {
        $bytes = [System.IO.File]::ReadAllBytes($full)
        if ($bytes.Length -ge 2 -and
            (($bytes[0] -eq 0xff -and $bytes[1] -eq 0xfe) -or
             ($bytes[0] -eq 0xfe -and $bytes[1] -eq 0xff))) {
            throw 'CONFIG_ENCODING_NOT_UTF8'
        }
        $offset = 0
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf) {
            $hasBom = $true
            $offset = 3
        }
        $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
        try {
            $text = $strictUtf8.GetString($bytes, $offset, $bytes.Length - $offset)
        } catch {
            throw 'CONFIG_ENCODING_NOT_STRICT_UTF8'
        }
        if ($text.IndexOf([char]0) -ge 0) { throw 'CONFIG_CONTAINS_NUL' }
    }

    $newline = "`r`n"
    $firstLf = $text.IndexOf("`n")
    if ($firstLf -ge 0) {
        $newline = $(if ($firstLf -gt 0 -and $text[$firstLf - 1] -eq "`r") { "`r`n" } else { "`n" })
    }
    $aclSddl = $null
    $attributes = $null
    if ($exists) {
        try { $aclSddl = (Get-Acl -LiteralPath $full).Sddl } catch { throw 'CONFIG_ACL_READ_FAILED' }
        $attributes = [int](Get-Item -Force -LiteralPath $full).Attributes
    }
    return [pscustomobject]@{
        schema = 'stage14-config-byte-snapshot/v1'
        path = $full
        exists = $exists
        bytes = $bytes
        text = $text
        has_utf8_bom = $hasBom
        newline = $newline
        sha256 = $(if ($exists) { Get-SmSha256Bytes -Bytes $bytes } else { $null })
        byte_count = $bytes.Length
        acl_sddl = $aclSddl
        attributes = $attributes
    }
}

function Write-SmJson {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Value
    )
    Write-SmUtf8NoBom -Path $Path -Text (($Value | ConvertTo-Json -Depth 20) + "`n")
}

function Read-SmJson {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing JSON file: $Path" }
    return ([System.IO.File]::ReadAllText($Path, [System.Text.UTF8Encoding]::new($false)) | ConvertFrom-Json)
}

function Get-SmDefaultInstallRoot {
    if ($env:SEMANTIC_MEMORY_HOME) {
        return [System.IO.Path]::GetFullPath($env:SEMANTIC_MEMORY_HOME)
    }
    $local = $env:LOCALAPPDATA
    if (-not $local) { $local = [Environment]::GetFolderPath('LocalApplicationData') }
    if (-not $local) { throw 'LOCALAPPDATA is unavailable.' }
    return [System.IO.Path]::GetFullPath((Join-Path $local 'SemanticMemory'))
}

function Test-SmSafeRelativePath {
    param([Parameter(Mandatory=$true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $false }
    $parts = $Path.Replace('/', '\').Split('\')
    return -not ($parts | Where-Object { $_ -eq '..' -or $_ -eq '' })
}

function Resolve-SmChildPath {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        [Parameter(Mandatory=$true)][string]$RelativePath
    )
    if (-not (Test-SmSafeRelativePath -Path $RelativePath)) { throw "Unsafe relative path: $RelativePath" }
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $full = [System.IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes root: $RelativePath"
    }
    return $full
}

function Remove-SmTomlComment {
    param([Parameter(Mandatory=$true)][string]$Line)
    $single = $false
    $double = $false
    $escaped = $false
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $c = $Line[$i]
        if ($double -and $c -eq '\' -and -not $escaped) { $escaped = $true; continue }
        if ($c -eq '"' -and -not $single -and -not $escaped) { $double = -not $double }
        elseif ($c -eq "'" -and -not $double) { $single = -not $single }
        elseif ($c -eq '#' -and -not $single -and -not $double) { return $Line.Substring(0, $i) }
        $escaped = $false
    }
    return $Line
}

function Split-SmTomlArray {
    param([Parameter(Mandatory=$true)][string]$Body)
    $values = New-Object System.Collections.Generic.List[string]
    $start = 0
    $single = $false
    $double = $false
    $escaped = $false
    for ($i = 0; $i -lt $Body.Length; $i++) {
        $c = $Body[$i]
        if ($double -and $c -eq '\' -and -not $escaped) { $escaped = $true; continue }
        if ($c -eq '"' -and -not $single -and -not $escaped) { $double = -not $double }
        elseif ($c -eq "'" -and -not $double) { $single = -not $single }
        elseif ($c -eq ',' -and -not $single -and -not $double) {
            $values.Add($Body.Substring($start, $i - $start).Trim())
            $start = $i + 1
        }
        $escaped = $false
    }
    if ($start -lt $Body.Length) { $values.Add($Body.Substring($start).Trim()) }
    return $values.ToArray()
}

function ConvertFrom-SmTomlValue {
    param([Parameter(Mandatory=$true)][string]$Value)
    $v = $Value.Trim()
    if ($v -eq 'true') { return $true }
    if ($v -eq 'false') { return $false }
    if ($v.StartsWith("'") -and $v.EndsWith("'")) {
        if ($v.Length -lt 2) { throw 'Unsupported managed TOML literal string.' }
        $inner = $v.Substring(1, $v.Length - 2)
        if ($inner.Contains("'") -or $inner.Contains("`r") -or $inner.Contains("`n")) {
            throw 'Unsupported managed TOML literal string.'
        }
        return $inner
    }
    if ($v.StartsWith('"') -and $v.EndsWith('"')) { return ($v | ConvertFrom-Json) }
    if ($v.StartsWith('[') -and $v.EndsWith(']')) {
        $body = $v.Substring(1, $v.Length - 2).Trim()
        if (-not $body) { return ,@() }
        return ,@(Split-SmTomlArray -Body $body | ForEach-Object { ConvertFrom-SmTomlValue -Value $_ })
    }
    $number = 0L
    if ([long]::TryParse($v, [ref]$number)) { return $number }
    throw "Unsupported managed TOML value: $v"
}

function ConvertFrom-SmTomlKeyPath {
    param([Parameter(Mandatory=$true)][string]$Text)
    $parts = New-Object System.Collections.Generic.List[string]
    $index = 0
    while ($index -lt $Text.Length) {
        while ($index -lt $Text.Length -and [char]::IsWhiteSpace($Text[$index])) { $index++ }
        if ($index -ge $Text.Length) { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
        $token = $null
        if ($Text[$index] -eq '"') {
            $start = $index
            $index++
            $escaped = $false
            $closed = $false
            while ($index -lt $Text.Length) {
                $c = $Text[$index]
                if ($c -eq '"' -and -not $escaped) {
                    $index++
                    $closed = $true
                    break
                }
                if ($c -eq '\' -and -not $escaped) { $escaped = $true } else { $escaped = $false }
                $index++
            }
            if (-not $closed) { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
            try { $token = ($Text.Substring($start, $index - $start) | ConvertFrom-Json) }
            catch { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
        } elseif ($Text[$index] -eq "'") {
            $index++
            $start = $index
            while ($index -lt $Text.Length -and $Text[$index] -ne "'") { $index++ }
            if ($index -ge $Text.Length) { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
            $token = $Text.Substring($start, $index - $start)
            $index++
        } else {
            $start = $index
            while ($index -lt $Text.Length -and
                   -not [char]::IsWhiteSpace($Text[$index]) -and
                   $Text[$index] -ne '.') {
                $index++
            }
            $token = $Text.Substring($start, $index - $start)
            if ($token -notmatch '^[A-Za-z0-9_-]+$') { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
        }
        if ([string]::IsNullOrEmpty([string]$token)) { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
        $parts.Add([string]$token)
        while ($index -lt $Text.Length -and [char]::IsWhiteSpace($Text[$index])) { $index++ }
        if ($index -ge $Text.Length) { break }
        if ($Text[$index] -ne '.') { throw 'CONFIG_TOML_KEY_PATH_INVALID' }
        $index++
    }
    return $parts.ToArray()
}

function Get-SmTomlStructuralPrefix {
    param(
        [Parameter(Mandatory=$true)][AllowEmptyString()][string]$Raw,
        [Parameter(Mandatory=$true)][ref]$MultilineDelimiter
    )
    $structural = [System.Text.StringBuilder]::new()
    $single = $false
    $double = $false
    $escaped = $false
    for ($index = 0; $index -lt $Raw.Length; $index++) {
        if ($MultilineDelimiter.Value) {
            if ($index + 2 -lt $Raw.Length -and
                $Raw.Substring($index, 3) -ceq [string]$MultilineDelimiter.Value) {
                $escapedDelimiter = $false
                if ([string]$MultilineDelimiter.Value -ceq '"""') {
                    $slashes = 0
                    for ($back = $index - 1; $back -ge 0 -and $Raw[$back] -eq '\'; $back--) { $slashes++ }
                    $escapedDelimiter = (($slashes % 2) -eq 1)
                }
                if (-not $escapedDelimiter) {
                    $MultilineDelimiter.Value = ''
                    $index += 2
                }
            }
            continue
        }
        if (-not $single -and -not $double -and $index + 2 -lt $Raw.Length) {
            $candidate = $Raw.Substring($index, 3)
            if ($candidate -ceq '"""' -or $candidate -ceq "'''") {
                $MultilineDelimiter.Value = $candidate
                $index += 2
                continue
            }
        }
        $c = $Raw[$index]
        if (-not $single -and -not $double -and $c -eq '#') { break }
        [void]$structural.Append($c)
        if ($double -and $c -eq '\' -and -not $escaped) {
            $escaped = $true
            continue
        }
        if ($c -eq '"' -and -not $single -and -not $escaped) { $double = -not $double }
        elseif ($c -eq "'" -and -not $double) { $single = -not $single }
        $escaped = $false
    }
    return $structural.ToString()
}

function Get-SmTomlUnquotedCharacterIndex {
    param(
        [Parameter(Mandatory=$true)][string]$Text,
        [Parameter(Mandatory=$true)][char]$Character,
        [int]$StartIndex = 0
    )
    $single = $false
    $double = $false
    $escaped = $false
    for ($index = $StartIndex; $index -lt $Text.Length; $index++) {
        $c = $Text[$index]
        if (-not $single -and -not $double -and $c -eq $Character) { return $index }
        if ($double -and $c -eq '\' -and -not $escaped) {
            $escaped = $true
            continue
        }
        if ($c -eq '"' -and -not $single -and -not $escaped) { $double = -not $double }
        elseif ($c -eq "'" -and -not $double) { $single = -not $single }
        $escaped = $false
    }
    return -1
}

function Test-SmPathPrefix {
    param([string[]]$Path,[string[]]$Prefix)
    if ($Path.Count -lt $Prefix.Count) { return $false }
    for ($index = 0; $index -lt $Prefix.Count; $index++) {
        if (-not [string]::Equals($Path[$index], $Prefix[$index], [System.StringComparison]::Ordinal)) {
            return $false
        }
    }
    return $true
}

function Get-SmManagedTomlLayout {
    param([Parameter(Mandatory=$true)]$Snapshot)
    $text = [string]$Snapshot.text
    $rootPath = @('mcp_servers','semantic_memory')
    $envPath = @('mcp_servers','semantic_memory','env')
    $allowedRoot = @('command','args','enabled')
    $allowedEnv = @(
        'CBM_DATA_ROOT',
        'CBM_MEMORY_AUTO_MAINTAIN',
        'CBM_MEMORY_EMBED_BACKEND',
        'CBM_MEMORY_NO_GLOBAL_UNION',
        'CBM_STAGE14_PRODUCTION_GATE',
        'CBM_STAGE14_EVOLUTION_MODE',
        'CBM_STAGE14_CANARY_AUTH_MANIFEST',
        'CBM_STAGE14_CANARY_AUTH_SHA256'
    )
    $values = @{}
    $spans = @{}
    $sections = @{ root = $null; env = $null }
    $seenRegularTables = @{}
    $rootIdentity = [string]::Join("`0", $rootPath)
    $envIdentity = [string]::Join("`0", $envPath)
    [string[]]$currentPath = @()
    $multilineDelimiter = ''
    $lineStart = 0

    while ($lineStart -lt $text.Length) {
        $lf = $text.IndexOf("`n", $lineStart)
        $lineEnd = $(if ($lf -ge 0) { $lf + 1 } else { $text.Length })
        $contentEnd = $(if ($lf -ge 0) { $lf } else { $text.Length })
        if ($contentEnd -gt $lineStart -and $text[$contentEnd - 1] -eq "`r") { $contentEnd-- }
        $raw = $text.Substring($lineStart, $contentEnd - $lineStart)
        $structural = Get-SmTomlStructuralPrefix -Raw $raw -MultilineDelimiter ([ref]$multilineDelimiter)
        $trimmed = $structural.Trim()

        $isHeader = $false
        if ($trimmed.StartsWith('[') -and -not $trimmed.EndsWith(']')) {
            # TOML table headers cannot span lines. Reject every structural line
            # that begins like a header but does not end as one; limiting this
            # check to the raw spelling of semantic_memory is bypassable through
            # quoted-key escapes such as semantic\u005fmemory.
            throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
        }
        if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) {
            $isArrayHeader = $trimmed.StartsWith('[[') -and $trimmed.EndsWith(']]')
            $inner = $(if ($isArrayHeader) {
                $trimmed.Substring(2, $trimmed.Length - 4)
            } else {
                $trimmed.Substring(1, $trimmed.Length - 2)
            })
            try { [string[]]$headerPath = @(ConvertFrom-SmTomlKeyPath -Text $inner) }
            catch {
                if ($inner -match '(?<![A-Za-z0-9_])semantic_memory(?![A-Za-z0-9_])') {
                    throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
                }
                $headerPath = @()
            }
            if ($headerPath.Count -gt 0) {
                $isHeader = $true
                $headerIdentity = [string]::Join("`0", $headerPath)
                if (-not $isArrayHeader) {
                    if ($seenRegularTables.ContainsKey($headerIdentity)) {
                        if ($headerIdentity -ceq $rootIdentity -or $headerIdentity -ceq $envIdentity) {
                            throw 'CONFIG_DUPLICATE_MANAGED_TABLE'
                        }
                        if ((Test-SmPathPrefix -Path $rootPath -Prefix $headerPath) -or
                            (Test-SmPathPrefix -Path $headerPath -Prefix $rootPath) -or
                            ($headerPath.Count -gt 0 -and $headerPath[0] -ceq 'mcp_servers')) {
                            throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
                        }
                        throw 'CONFIG_TOML_DUPLICATE_TABLE'
                    }
                    $seenRegularTables[$headerIdentity] = $true
                }
                if (Test-SmPathPrefix -Path $headerPath -Prefix $rootPath) {
                    if ($isArrayHeader) { throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE' }
                    if ($headerPath.Count -eq 2) {
                        if ($sections.root) { throw 'CONFIG_DUPLICATE_MANAGED_TABLE' }
                        $sections.root = [pscustomobject]@{
                            header_start = $lineStart
                            header_end = $lineEnd
                            header_has_newline = ($lf -ge 0)
                        }
                    } elseif ($headerPath.Count -eq 3 -and $headerPath[2] -ceq 'env') {
                        if ($sections.env) { throw 'CONFIG_DUPLICATE_MANAGED_TABLE' }
                        $sections.env = [pscustomobject]@{
                            header_start = $lineStart
                            header_end = $lineEnd
                            header_has_newline = ($lf -ge 0)
                        }
                    } else {
                        throw 'CONFIG_UNKNOWN_MANAGED_SUBTABLE'
                    }
                }
                $currentPath = $headerPath
            }
        }

        if (-not $isHeader -and $trimmed) {
            $equalsIndex = Get-SmTomlUnquotedCharacterIndex -Text $structural -Character '='
            if ($equalsIndex -ge 1) {
                $keyText = $structural.Substring(0, $equalsIndex).Trim()
                try { [string[]]$keyPath = @(ConvertFrom-SmTomlKeyPath -Text $keyText) }
                catch {
                    if ((Test-SmPathPrefix -Path $currentPath -Prefix $rootPath) -or
                        $keyText -match '(?<![A-Za-z0-9_])semantic_memory(?![A-Za-z0-9_])') {
                        throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
                    }
                    $keyPath = @()
                }
                if ($keyPath.Count -gt 0) {
                    [string[]]$fullPath = @($currentPath) + @($keyPath)
                    $insideRoot = ($currentPath.Count -eq 2 -and (Test-SmPathPrefix -Path $currentPath -Prefix $rootPath))
                    $insideEnv = ($currentPath.Count -eq 3 -and (Test-SmPathPrefix -Path $currentPath -Prefix $envPath))
                    if ($insideRoot -or $insideEnv) {
                        if ($keyPath.Count -ne 1) { throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE' }
                        $key = $keyPath[0]
                        $allowed = $(if ($insideRoot) { $allowedRoot } else { $allowedEnv })
                        if ($allowed -cnotcontains $key) { throw "CONFIG_UNKNOWN_MANAGED_KEY:$key" }
                        $sectionName = $(if ($insideRoot) { 'mcp_servers.semantic_memory' } else { 'mcp_servers.semantic_memory.env' })
                        $field = "$sectionName.$key"
                        if ($values.ContainsKey($field)) { throw "CONFIG_DUPLICATE_MANAGED_KEY:$field" }
                        $valueStart = $equalsIndex + 1
                        while ($valueStart -lt $raw.Length -and [char]::IsWhiteSpace($raw[$valueStart])) { $valueStart++ }
                        $commentIndex = Get-SmTomlUnquotedCharacterIndex -Text $raw -Character '#' -StartIndex $valueStart
                        $valueEnd = $(if ($commentIndex -ge 0) { $commentIndex } else { $raw.Length })
                        while ($valueEnd -gt $valueStart -and [char]::IsWhiteSpace($raw[$valueEnd - 1])) { $valueEnd-- }
                        if ($valueEnd -le $valueStart -or $raw.Substring($valueStart, $valueEnd - $valueStart).Contains('"""') -or
                            $raw.Substring($valueStart, $valueEnd - $valueStart).Contains("'''")) {
                            throw "CONFIG_UNSUPPORTED_MANAGED_VALUE:$field"
                        }
                        $rawValue = $raw.Substring($valueStart, $valueEnd - $valueStart)
                        try { $parsedValue = ConvertFrom-SmTomlValue -Value $rawValue }
                        catch { throw "CONFIG_UNSUPPORTED_MANAGED_VALUE:$field" }
                        $values[$field] = $parsedValue
                        $spans[$field] = [pscustomobject]@{
                            start = $lineStart + $valueStart
                            length = $valueEnd - $valueStart
                            line_start = $lineStart
                            line_end = $lineEnd
                        }
                    } elseif ((Test-SmPathPrefix -Path $fullPath -Prefix $rootPath) -or
                              ($fullPath.Count -eq 1 -and $fullPath[0] -ceq 'mcp_servers')) {
                        throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
                    }
                }
            } elseif (Test-SmPathPrefix -Path $currentPath -Prefix $rootPath) {
                throw 'CONFIG_AMBIGUOUS_MANAGED_SHAPE'
            }
        }
        $lineStart = $lineEnd
    }
    if ($multilineDelimiter) { throw 'CONFIG_UNTERMINATED_MULTILINE_STRING' }
    return [pscustomobject]@{
        schema = 'stage14-managed-toml-layout/v1'
        values = $values
        spans = $spans
        sections = $sections
    }
}

function ConvertTo-SmTomlBasicString {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string]$Value)
    return ($Value | ConvertTo-Json -Compress)
}

function ConvertTo-SmManagedTomlValue {
    param([Parameter(Mandatory=$true)][string]$Field,[Parameter(Mandatory=$true)]$Value)
    if ($Field -eq 'mcp_servers.semantic_memory.args') {
        if (@($Value).Count -ne 0) { throw 'CONFIG_DESIRED_ARGS_MUST_BE_EMPTY' }
        return '[]'
    }
    if ($Field -eq 'mcp_servers.semantic_memory.enabled') {
        if ($Value -isnot [bool] -or -not [bool]$Value) { throw 'CONFIG_DESIRED_ENABLED_MUST_BE_TRUE' }
        return 'true'
    }
    return ConvertTo-SmTomlBasicString -Value ([string]$Value)
}

function ConvertTo-SmSnapshotBytes {
    param([Parameter(Mandatory=$true)][string]$Text,[Parameter(Mandatory=$true)][bool]$HasUtf8Bom)
    $utf8 = [System.Text.UTF8Encoding]::new($false, $true)
    [byte[]]$content = $utf8.GetBytes($Text)
    if (-not $HasUtf8Bom) { return $content }
    [byte[]]$result = New-Object byte[] ($content.Length + 3)
    $result[0] = 0xef
    $result[1] = 0xbb
    $result[2] = 0xbf
    [System.Buffer]::BlockCopy($content, 0, $result, 3, $content.Length)
    return $result
}

function Read-SmManagedToml {
    param([Parameter(Mandatory=$true)][string]$Path)
    $snapshot = Get-SmConfigByteSnapshot -Path $Path
    return (Get-SmManagedTomlLayout -Snapshot $snapshot).values
}

function Convert-SmCanonicalPath {
    param([string]$Path)
    if (-not $Path) { return $Path }
    if (-not [System.IO.Path]::IsPathRooted($Path)) { return $Path.Replace('/', '\') }
    $full = [System.IO.Path]::GetFullPath($Path).Replace('/', '\')
    $root = [System.IO.Path]::GetPathRoot($full)
    if ($full.Length -gt $root.Length) { $full = $full.TrimEnd('\') }
    return $full.ToLowerInvariant()
}

function Get-SmManagedConfigStateFromSnapshot {
    param([Parameter(Mandatory=$true)]$Snapshot)
    $parsed = (Get-SmManagedTomlLayout -Snapshot $Snapshot).values
    $requiredNames = @(
        'mcp_servers.semantic_memory.command',
        'mcp_servers.semantic_memory.args',
        'mcp_servers.semantic_memory.enabled',
        'mcp_servers.semantic_memory.env.CBM_DATA_ROOT',
        'mcp_servers.semantic_memory.env.CBM_MEMORY_AUTO_MAINTAIN',
        'mcp_servers.semantic_memory.env.CBM_MEMORY_EMBED_BACKEND'
    )
    $optionalNames = @(
        'mcp_servers.semantic_memory.env.CBM_MEMORY_NO_GLOBAL_UNION',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256'
    )
    $names = @($requiredNames) + @($optionalNames)
    $fields = [ordered]@{}
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($name in $names) {
        if (-not $parsed.ContainsKey($name)) {
            if ($requiredNames -ccontains $name) { $missing.Add($name) }
            continue
        }
        $value = $parsed[$name]
        if ($name -eq 'mcp_servers.semantic_memory.command' -or
            $name.EndsWith('.CBM_DATA_ROOT') -or
            $name.EndsWith('.CBM_STAGE14_CANARY_AUTH_MANIFEST')) {
            $value = Convert-SmCanonicalPath -Path ([string]$value)
        }
        $fields[$name] = $value
    }
    $canonical = [ordered]@{ schema = 'stage14-managed-config-state/v1'; fields = $fields }
    $json = $canonical | ConvertTo-Json -Depth 12 -Compress
    return [pscustomobject]@{
        schema = 'stage14-managed-config-state/v1'
        complete = ($missing.Count -eq 0)
        missing_fields = $missing.ToArray()
        fields = $fields
        canonical_json = $json
        managed_fingerprint_sha256 = Get-SmSha256Text -Text $json
        whole_config_sha256 = $Snapshot.sha256
    }
}

function Get-SmManagedConfigState {
    param([Parameter(Mandatory=$true)][string]$ConfigPath)
    $snapshot = Get-SmConfigByteSnapshot -Path $ConfigPath
    return Get-SmManagedConfigStateFromSnapshot -Snapshot $snapshot
}

function Compare-SmManagedConfig {
    param(
        [Parameter(Mandatory=$true)][string]$ReferenceConfigPath,
        [Parameter(Mandatory=$true)][string]$CandidateConfigPath
    )
    $reference = Get-SmManagedConfigState -ConfigPath $ReferenceConfigPath
    $candidate = Get-SmManagedConfigState -ConfigPath $CandidateConfigPath
    $changed = New-Object System.Collections.Generic.List[string]
    foreach ($name in $reference.fields.Keys) {
        if (-not $candidate.fields.Contains($name)) { $changed.Add($name); continue }
        $a = $reference.fields[$name] | ConvertTo-Json -Depth 8 -Compress
        $b = $candidate.fields[$name] | ConvertTo-Json -Depth 8 -Compress
        if ($a -cne $b) { $changed.Add($name) }
    }
    foreach ($name in $candidate.fields.Keys) {
        if (-not $reference.fields.Contains($name)) { $changed.Add($name) }
    }
    $class = 'GREEN_UNRELATED_DRIFT'
    if (-not $reference.complete -or -not $candidate.complete -or $changed.Count -gt 0) {
        $class = 'RED_MANAGED_CONFIG_DRIFT'
    }
    return [pscustomobject]@{
        schema = 'stage14-managed-config-drift/v1'
        classification = $class
        managed_fingerprint_before = $reference.managed_fingerprint_sha256
        managed_fingerprint_after = $candidate.managed_fingerprint_sha256
        whole_config_sha256_before = $reference.whole_config_sha256
        whole_config_sha256_after = $candidate.whole_config_sha256
        changed_managed_fields = @($changed | Sort-Object -Unique)
        missing_managed_fields = @($candidate.missing_fields)
        raw_credential_values_recorded = $false
    }
}

function Get-SmDesiredManagedConfigState {
    param(
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [switch]$EnableProductionCanary,
        [AllowEmptyString()][string]$CanaryAuthManifestPath,
        [AllowEmptyString()][string]$CanaryAuthSha256
    )
    $root = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($InstallRoot)) -AllowMissingLeaf
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'CONFIG_INSTALL_ROOT_MISSING' }
    $current = Get-SmCurrentPayloadFast -InstallRoot $root
    $command = Join-Path $root 'bin\semantic-memory-mcp.exe'
    $dataRoot = Join-Path $root 'data'
    if (-not (Test-Path -LiteralPath $command -PathType Leaf)) { throw 'CONFIG_STABLE_MCP_MISSING' }
    if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) { throw 'CONFIG_DATA_ROOT_MISSING' }
    $mcpRelative = [string]$current.verification.manifest.entrypoints.mcp
    $mcpRecord = @($current.verification.manifest.files | Where-Object { [string]$_.path -ceq $mcpRelative })
    if ($mcpRecord.Count -ne 1 -or
        (Get-SmSha256File -Path $command) -ne ([string]$mcpRecord[0].sha256).ToLowerInvariant()) {
        throw 'RED_PAYLOAD_INTEGRITY_FAILURE: stable MCP binding mismatch.'
    }
    $fields = [ordered]@{
        'mcp_servers.semantic_memory.command' = [System.IO.Path]::GetFullPath($command)
        'mcp_servers.semantic_memory.args' = @()
        'mcp_servers.semantic_memory.enabled' = $true
        'mcp_servers.semantic_memory.env.CBM_DATA_ROOT' = [System.IO.Path]::GetFullPath($dataRoot)
        'mcp_servers.semantic_memory.env.CBM_MEMORY_AUTO_MAINTAIN' = '0'
        'mcp_servers.semantic_memory.env.CBM_MEMORY_EMBED_BACKEND' = 'static'
    }
    $canary = [ordered]@{
        enabled = $false
        auth_manifest_path = $null
        auth_manifest_sha256 = $null
    }
    $canaryArgumentsPresent = -not [string]::IsNullOrWhiteSpace($CanaryAuthManifestPath) -or
        -not [string]::IsNullOrWhiteSpace($CanaryAuthSha256)
    if ($EnableProductionCanary) {
        if ([string]::IsNullOrWhiteSpace($CanaryAuthManifestPath) -or
            [string]::IsNullOrWhiteSpace($CanaryAuthSha256)) {
            throw 'CONFIG_CANARY_ARGUMENTS_INCOMPLETE'
        }
        if (-not [System.IO.Path]::IsPathRooted($CanaryAuthManifestPath) -or
            $CanaryAuthSha256 -notmatch '^[0-9a-f]{64}$') {
            throw 'CONFIG_CANARY_AUTH_BINDING_INVALID'
        }
        $authPath = Assert-SmNoReparsePath -Path (
            [System.IO.Path]::GetFullPath($CanaryAuthManifestPath)
        )
        if (-not (Test-Path -LiteralPath $authPath -PathType Leaf) -or
            (Get-SmSha256File -Path $authPath) -cne $CanaryAuthSha256) {
            throw 'CONFIG_CANARY_AUTH_BINDING_MISMATCH'
        }
        $fields['mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE'] = '1'
        $fields['mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE'] = 'bounded_canary'
        $fields['mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST'] =
            [System.IO.Path]::GetFullPath($authPath)
        $fields['mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256'] =
            $CanaryAuthSha256
        $canary.enabled = $true
        $canary.auth_manifest_path = [System.IO.Path]::GetFullPath($authPath)
        $canary.auth_manifest_sha256 = $CanaryAuthSha256
    } elseif ($canaryArgumentsPresent) {
        throw 'CONFIG_CANARY_ENABLE_SWITCH_REQUIRED'
    }
    return [pscustomobject]@{
        schema = 'stage14-desired-managed-config/v1'
        install_root = $root
        fields = $fields
        canary = $canary
    }
}

function Test-SmManagedValueEqual {
    param([Parameter(Mandatory=$true)][string]$Field,$Actual,$Expected)
    if ($Field -eq 'mcp_servers.semantic_memory.command' -or
        $Field.EndsWith('.CBM_DATA_ROOT') -or
        $Field.EndsWith('.CBM_STAGE14_CANARY_AUTH_MANIFEST')) {
        try {
            return (Convert-SmCanonicalPath -Path ([string]$Actual)) -ceq
                   (Convert-SmCanonicalPath -Path ([string]$Expected))
        } catch { return $false }
    }
    if ($Field -eq 'mcp_servers.semantic_memory.args') {
        return (($Actual | Measure-Object).Count -eq 0) -and
               (($Expected | Measure-Object).Count -eq 0)
    }
    if ($Field -eq 'mcp_servers.semantic_memory.enabled') {
        return ($Actual -is [bool]) -and ($Expected -is [bool]) -and
               ([bool]$Actual -eq [bool]$Expected)
    }
    return ([string]$Actual) -ceq ([string]$Expected)
}

function New-SmManagedSectionText {
    param(
        [Parameter(Mandatory=$true)][ValidateSet('root','env')][string]$Section,
        [Parameter(Mandatory=$true)]$DesiredFields,
        [Parameter(Mandatory=$true)][string]$Newline
    )
    if ($Section -eq 'root') {
        $header = '[mcp_servers.semantic_memory]'
        $names = @(
            'mcp_servers.semantic_memory.command',
            'mcp_servers.semantic_memory.args',
            'mcp_servers.semantic_memory.enabled'
        )
    } else {
        $header = '[mcp_servers.semantic_memory.env]'
        $names = @(
            'mcp_servers.semantic_memory.env.CBM_DATA_ROOT',
            'mcp_servers.semantic_memory.env.CBM_MEMORY_AUTO_MAINTAIN',
            'mcp_servers.semantic_memory.env.CBM_MEMORY_EMBED_BACKEND',
            'mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE',
            'mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE',
            'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST',
            'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256'
        )
    }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append($header).Append($Newline)
    foreach ($field in $names) {
        if (-not $DesiredFields.Contains($field)) { continue }
        $key = $field.Substring($field.LastIndexOf('.') + 1)
        $serialized = ConvertTo-SmManagedTomlValue -Field $field -Value $DesiredFields[$field]
        [void]$builder.Append($key).Append(' = ').Append($serialized).Append($Newline)
    }
    return $builder.ToString()
}

function New-SmManagedConfigPatch {
    param(
        [Parameter(Mandatory=$true)]$Snapshot,
        [Parameter(Mandatory=$true)]$DesiredState
    )
    $layout = Get-SmManagedTomlLayout -Snapshot $Snapshot
    $desired = $DesiredState.fields
    $rootFields = @(
        'mcp_servers.semantic_memory.command',
        'mcp_servers.semantic_memory.args',
        'mcp_servers.semantic_memory.enabled'
    )
    $requiredEnvFields = @(
        'mcp_servers.semantic_memory.env.CBM_DATA_ROOT',
        'mcp_servers.semantic_memory.env.CBM_MEMORY_AUTO_MAINTAIN',
        'mcp_servers.semantic_memory.env.CBM_MEMORY_EMBED_BACKEND'
    )
    $optionalCanaryFields = @(
        'mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256'
    )
    $forbiddenEnvFields = @(
        'mcp_servers.semantic_memory.env.CBM_MEMORY_NO_GLOBAL_UNION'
    )
    $envFields = @($requiredEnvFields) + @(
        $optionalCanaryFields | Where-Object { $desired.Contains($_) }
    )
    $allDesiredFields = @($rootFields) + @($envFields)
    foreach ($field in @($rootFields) + @($requiredEnvFields)) {
        if (-not $desired.Contains($field)) { throw "CONFIG_DESIRED_FIELD_MISSING:$field" }
    }
    foreach ($field in $allDesiredFields) {
        [void](ConvertTo-SmManagedTomlValue -Field $field -Value $desired[$field])
    }

    $edits = New-Object System.Collections.Generic.List[object]
    $changed = New-Object System.Collections.Generic.List[string]
    foreach ($field in $allDesiredFields) {
        if ($layout.values.ContainsKey($field)) {
            if (-not (Test-SmManagedValueEqual -Field $field -Actual $layout.values[$field] -Expected $desired[$field])) {
                $span = $layout.spans[$field]
                $edits.Add([pscustomobject]@{
                    start = [int]$span.start
                    length = [int]$span.length
                    replacement = ConvertTo-SmManagedTomlValue -Field $field -Value $desired[$field]
                    fields = @($field)
                })
                $changed.Add($field)
            }
        }
    }
    foreach ($field in @($optionalCanaryFields) + @($forbiddenEnvFields)) {
        if ($desired.Contains($field) -or -not $layout.values.ContainsKey($field)) { continue }
        $span = $layout.spans[$field]
        $edits.Add([pscustomobject]@{
            start = [int]$span.line_start
            length = [int]$span.line_end - [int]$span.line_start
            replacement = ''
            fields = @($field)
        })
        $changed.Add($field)
    }

    $newline = [string]$Snapshot.newline
    if (-not $layout.sections.root -and -not $layout.sections.env) {
        $separator = ''
        if ($Snapshot.text.Length -gt 0) {
            $separator = $(if ($Snapshot.text.EndsWith("`n")) { $newline } else { $newline + $newline })
        }
        $replacement = $separator +
            (New-SmManagedSectionText -Section root -DesiredFields $desired -Newline $newline) +
            $newline +
            (New-SmManagedSectionText -Section env -DesiredFields $desired -Newline $newline)
        $edits.Add([pscustomobject]@{
            start = $Snapshot.text.Length
            length = 0
            replacement = $replacement
            fields = $allDesiredFields
        })
        foreach ($field in $allDesiredFields) { $changed.Add($field) }
    } else {
        if (-not $layout.sections.root) {
            $rootText = New-SmManagedSectionText -Section root -DesiredFields $desired -Newline $newline
            $edits.Add([pscustomobject]@{
                start = [int]$layout.sections.env.header_start
                length = 0
                replacement = $rootText + $newline
                fields = $rootFields
            })
            foreach ($field in $rootFields) { $changed.Add($field) }
        } else {
            $missingRoot = @($rootFields | Where-Object { -not $layout.values.ContainsKey($_) })
            if ($missingRoot.Count -gt 0) {
                $insert = $(if ($layout.sections.root.header_has_newline) { '' } else { $newline })
                foreach ($field in $missingRoot) {
                    $key = $field.Substring($field.LastIndexOf('.') + 1)
                    $insert += "$key = $(ConvertTo-SmManagedTomlValue -Field $field -Value $desired[$field])$newline"
                    $changed.Add($field)
                }
                $edits.Add([pscustomobject]@{
                    start = [int]$layout.sections.root.header_end
                    length = 0
                    replacement = $insert
                    fields = $missingRoot
                })
            }
        }
        if (-not $layout.sections.env) {
            $separator = $(if ($Snapshot.text.Length -eq 0) { '' }
                elseif ($Snapshot.text.EndsWith("`n")) { $newline }
                else { $newline + $newline })
            $envText = New-SmManagedSectionText -Section env -DesiredFields $desired -Newline $newline
            $edits.Add([pscustomobject]@{
                start = $Snapshot.text.Length
                length = 0
                replacement = $separator + $envText
                fields = $envFields
            })
            foreach ($field in $envFields) { $changed.Add($field) }
        } else {
            $missingEnv = @($envFields | Where-Object { -not $layout.values.ContainsKey($_) })
            if ($missingEnv.Count -gt 0) {
                $insert = $(if ($layout.sections.env.header_has_newline) { '' } else { $newline })
                foreach ($field in $missingEnv) {
                    $key = $field.Substring($field.LastIndexOf('.') + 1)
                    $insert += "$key = $(ConvertTo-SmManagedTomlValue -Field $field -Value $desired[$field])$newline"
                    $changed.Add($field)
                }
                $edits.Add([pscustomobject]@{
                    start = [int]$layout.sections.env.header_end
                    length = 0
                    replacement = $insert
                    fields = $missingEnv
                })
            }
        }
    }

    # Coalesce same-offset insertions in construction order. This removes any
    # dependency on Sort-Object tie ordering for header-only managed tables.
    $coalesced = New-Object System.Collections.Generic.List[object]
    foreach ($edit in $edits.ToArray()) {
        $existingInsertion = $null
        if ([int]$edit.length -eq 0) {
            $existingInsertion = $coalesced.ToArray() | Where-Object {
                [int]$_.length -eq 0 -and [int]$_.start -eq [int]$edit.start
            } | Select-Object -First 1
        }
        if ($null -ne $existingInsertion) {
            $existingInsertion.replacement =
                [string]$existingInsertion.replacement + [string]$edit.replacement
            $existingInsertion.fields =
                @($existingInsertion.fields) + @($edit.fields)
        } else {
            $coalesced.Add([pscustomobject]@{
                start = [int]$edit.start
                length = [int]$edit.length
                replacement = [string]$edit.replacement
                fields = @($edit.fields)
            })
        }
    }
    $ascending = @($coalesced.ToArray() | Sort-Object start,length)
    $cursor = 0
    $projection = [System.Text.StringBuilder]::new()
    foreach ($edit in $ascending) {
        if ([int]$edit.start -lt $cursor) { throw 'CONFIG_MANAGED_EDIT_OVERLAP' }
        [void]$projection.Append($Snapshot.text.Substring($cursor, [int]$edit.start - $cursor))
        $cursor = [int]$edit.start + [int]$edit.length
    }
    [void]$projection.Append($Snapshot.text.Substring($cursor))
    $projectionHash = Get-SmSha256Text -Text (
        "bom=$([bool]$Snapshot.has_utf8_bom)`n" + $projection.ToString()
    )

    $candidateText = [string]$Snapshot.text
    foreach ($edit in @($ascending | Sort-Object start -Descending)) {
        $candidateText = $candidateText.Substring(0, [int]$edit.start) +
            [string]$edit.replacement +
            $candidateText.Substring([int]$edit.start + [int]$edit.length)
    }
    [byte[]]$candidateBytes = ConvertTo-SmSnapshotBytes -Text $candidateText -HasUtf8Bom ([bool]$Snapshot.has_utf8_bom)
    $candidateSnapshot = [pscustomobject]@{
        text = $candidateText
        has_utf8_bom = [bool]$Snapshot.has_utf8_bom
        sha256 = Get-SmSha256Bytes -Bytes $candidateBytes
    }
    $candidateState = Get-SmManagedConfigStateFromSnapshot -Snapshot $candidateSnapshot
    if (-not $candidateState.complete) { throw 'CONFIG_CANDIDATE_MANAGED_FIELDS_INCOMPLETE' }
    foreach ($field in $allDesiredFields) {
        if (-not $candidateState.fields.Contains($field) -or
            -not (Test-SmManagedValueEqual -Field $field -Actual $candidateState.fields[$field] -Expected $desired[$field])) {
            throw "CONFIG_CANDIDATE_MANAGED_VALUE_MISMATCH:$field"
        }
    }
    foreach ($field in @($optionalCanaryFields) + @($forbiddenEnvFields)) {
        if (-not $desired.Contains($field) -and $candidateState.fields.Contains($field)) {
            throw "CONFIG_CANDIDATE_FORBIDDEN_ENV_FIELD:$field"
        }
    }

    return [pscustomobject]@{
        schema = 'stage14-managed-config-patch/v1'
        snapshot = $Snapshot
        desired_state = $DesiredState
        candidate_bytes = $candidateBytes
        candidate_sha256 = $candidateSnapshot.sha256
        candidate_managed_fingerprint_sha256 = $candidateState.managed_fingerprint_sha256
        changed_managed_fields = @($changed | Sort-Object -Unique)
        edit_count = $ascending.Count
        unmanaged_projection_sha256_before = $projectionHash
        unmanaged_projection_sha256_after = $projectionHash
        unmanaged_bytes_preserved = $true
        replay = ($ascending.Count -eq 0)
        raw_credential_values_recorded = $false
    }
}

function Get-SmSafeConfigErrorCode {
    param([Parameter(Mandatory=$true)][string]$Message)
    $prefix = $Message.Split(':')[0].Trim()
    if ($prefix -match '^(CONFIG|RED|FAULT_INJECTION)_[A-Z0-9_]+$') {
        return $prefix
    }
    if ($Message -match 'being used by another process|cannot access the file') {
        return 'CONFIG_FILE_LOCKED'
    }
    if ($Message -match 'Access.*denied|UnauthorizedAccess') {
        return 'CONFIG_ACCESS_DENIED'
    }
    return 'CONFIG_TRANSACTION_FAILED'
}

function Test-SmFullPathWithin {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Root
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    if ([string]::Equals($full, $rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $rootFull.TrimEnd('\') + '\'
    return $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Write-SmAtomicJsonSafe {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Value
    )
    $full = Assert-SmNoReparsePath -Path $Path -AllowMissingLeaf
    $parent = Split-Path -Parent $full
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw 'CONFIG_JOURNAL_PARENT_MISSING'
    }
    [byte[]]$bytes = [System.Text.UTF8Encoding]::new($false, $true).GetBytes(
        (($Value | ConvertTo-Json -Depth 20) + "`n")
    )
    $temporary = Join-Path $parent (
        '.{0}.semantic-memory-journal.{1}.tmp' -f
        [System.IO.Path]::GetFileName($full),
        [guid]::NewGuid().ToString('N')
    )
    Write-SmBytesCreateNew -Path $temporary -Bytes $bytes
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $previous = Join-Path $parent (
            '.{0}.semantic-memory-journal-previous.{1}.json' -f
            [System.IO.Path]::GetFileName($full),
            [guid]::NewGuid().ToString('N')
        )
        [System.IO.File]::Replace($temporary, $full, $previous, $false)
    } else {
        [System.IO.File]::Move($temporary, $full)
    }
    if ((Get-SmSha256File -Path $full) -ne (Get-SmSha256Bytes -Bytes $bytes)) {
        throw 'CONFIG_JOURNAL_VERIFY_FAILED'
    }
}

function Test-SmConfigSnapshotExpectation {
    param(
        [Parameter(Mandatory=$true)]$Snapshot,
        [AllowEmptyString()][string]$ExpectedConfigSha256
    )
    if ([string]::IsNullOrWhiteSpace($ExpectedConfigSha256)) {
        throw 'CONFIG_EXPECTED_SHA256_REQUIRED'
    }
    if ($ExpectedConfigSha256 -ceq 'ABSENT') { return -not [bool]$Snapshot.exists }
    if ($ExpectedConfigSha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'CONFIG_EXPECTED_SHA256_INVALID'
    }
    return [bool]$Snapshot.exists -and
        ([string]$Snapshot.sha256 -ceq $ExpectedConfigSha256)
}

function Test-SmConfigSnapshotsEqual {
    param(
        [Parameter(Mandatory=$true)]$Left,
        [Parameter(Mandatory=$true)]$Right
    )
    if ([bool]$Left.exists -ne [bool]$Right.exists) { return $false }
    if (-not [bool]$Left.exists) { return $true }
    if ([string]$Left.sha256 -cne [string]$Right.sha256) { return $false }
    if ($Left.PSObject.Properties.Name -contains 'acl_sddl' -and
        $Right.PSObject.Properties.Name -contains 'acl_sddl' -and
        [string]$Left.acl_sddl -cne [string]$Right.acl_sddl) {
        return $false
    }
    if ($Left.PSObject.Properties.Name -contains 'attributes' -and
        $Right.PSObject.Properties.Name -contains 'attributes' -and
        $null -ne $Left.attributes -and $null -ne $Right.attributes -and
        [int]$Left.attributes -ne [int]$Right.attributes) {
        return $false
    }
    return $true
}

function Test-SmExpectedCandidateSnapshot {
    param(
        [Parameter(Mandatory=$true)]$Snapshot,
        [Parameter(Mandatory=$true)][string]$CandidateSha256,
        $ExpectedCandidateSnapshot
    )
    if (-not [bool]$Snapshot.exists -or
        [string]$Snapshot.sha256 -cne $CandidateSha256) {
        return $false
    }
    if ($null -eq $ExpectedCandidateSnapshot) { return $true }
    return Test-SmConfigSnapshotsEqual `
        -Left $ExpectedCandidateSnapshot `
        -Right $Snapshot
}

function Get-SmRecoveryArtifactIntentPaths {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$TransactionId
    )
    $parent = Split-Path -Parent $ConfigPath
    $fileName = [System.IO.Path]::GetFileName($ConfigPath)
    $rollbackSource = Join-Path $parent (
        '.{0}.semantic-memory.{1}.rollback-source' -f
        $fileName,
        $TransactionId
    )
    $failedCandidate = Join-Path $parent (
        '.{0}.semantic-memory.{1}.failed-candidate' -f
        $fileName,
        $TransactionId
    )
    $compensation = @(
        for ($depth = 0; $depth -lt 8; $depth++) {
            Join-Path $parent (
                '.{0}.semantic-memory.{1}.compensation-{2}' -f
                $fileName,
                $TransactionId,
                $depth
            )
        }
    )
    return [pscustomobject]@{
        rollback_source = $rollbackSource
        failed_candidate = $failedCandidate
        compensation = @($compensation)
        all = @($rollbackSource, $failedCandidate) + @($compensation)
    }
}

function Test-SmManagedSnapshotMatchesDesired {
    param(
        [Parameter(Mandatory=$true)]$Snapshot,
        [Parameter(Mandatory=$true)]$DesiredState
    )
    if (-not [bool]$Snapshot.exists) { return $false }
    $state = Get-SmManagedConfigStateFromSnapshot -Snapshot $Snapshot
    if (-not $state.complete) { return $false }
    foreach ($field in $DesiredState.fields.Keys) {
        if (-not $state.fields.Contains($field) -or
            -not (Test-SmManagedValueEqual -Field $field -Actual $state.fields[$field] -Expected $DesiredState.fields[$field])) {
            return $false
        }
    }
    foreach ($field in @(
        'mcp_servers.semantic_memory.env.CBM_MEMORY_NO_GLOBAL_UNION',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST',
        'mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256'
    )) {
        if (-not $DesiredState.fields.Contains($field) -and $state.fields.Contains($field)) {
            return $false
        }
    }
    return $true
}

function Get-SmManagedConfigRepairPreview {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [switch]$EnableProductionCanary,
        [AllowEmptyString()][string]$CanaryAuthManifestPath,
        [AllowEmptyString()][string]$CanaryAuthSha256
    )
    $desired = Get-SmDesiredManagedConfigState `
        -InstallRoot $InstallRoot `
        -EnableProductionCanary:$EnableProductionCanary `
        -CanaryAuthManifestPath $CanaryAuthManifestPath `
        -CanaryAuthSha256 $CanaryAuthSha256
    $snapshot = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
    $patch = New-SmManagedConfigPatch -Snapshot $snapshot -DesiredState $desired
    $beforeState = Get-SmManagedConfigStateFromSnapshot -Snapshot $snapshot
    return [pscustomobject][ordered]@{
        schema = 'stage14-config-repair-preview/v2'
        mode = 'Preview'
        classification = $(if ($patch.replay) { 'GREEN_MANAGED_CONFIG_CURRENT' } else { 'RED_MANAGED_CONFIG_DRIFT' })
        replay = [bool]$patch.replay
        config_exists_before = [bool]$snapshot.exists
        whole_config_sha256_before = $snapshot.sha256
        whole_config_sha256_candidate = $patch.candidate_sha256
        managed_fingerprint_before = $beforeState.managed_fingerprint_sha256
        managed_fingerprint_candidate = $patch.candidate_managed_fingerprint_sha256
        changed_managed_fields = @($patch.changed_managed_fields)
        edit_count = [int]$patch.edit_count
        unmanaged_projection_sha256_before = $patch.unmanaged_projection_sha256_before
        unmanaged_projection_sha256_after = $patch.unmanaged_projection_sha256_after
        unmanaged_bytes_preserved = [bool]$patch.unmanaged_bytes_preserved
        production_canary_enabled = [bool]$desired.canary.enabled
        raw_credential_values_recorded = $false
        apply_performed = $false
    }
}

function Test-SmManagedConfigRepair {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [switch]$EnableProductionCanary,
        [AllowEmptyString()][string]$CanaryAuthManifestPath,
        [AllowEmptyString()][string]$CanaryAuthSha256
    )
    $desired = Get-SmDesiredManagedConfigState `
        -InstallRoot $InstallRoot `
        -EnableProductionCanary:$EnableProductionCanary `
        -CanaryAuthManifestPath $CanaryAuthManifestPath `
        -CanaryAuthSha256 $CanaryAuthSha256
    $snapshot = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
    $patch = New-SmManagedConfigPatch -Snapshot $snapshot -DesiredState $desired
    $state = Get-SmManagedConfigStateFromSnapshot -Snapshot $snapshot
    $matches = [bool]$snapshot.exists -and [bool]$patch.replay -and
        (Test-SmManagedSnapshotMatchesDesired -Snapshot $snapshot -DesiredState $desired)
    return [pscustomobject][ordered]@{
        schema = 'stage14-config-repair-verification/v2'
        mode = 'Verify'
        status = $(if ($matches) { 'PASS' } else { 'RED_MANAGED_CONFIG_DRIFT' })
        managed_fields_match = $matches
        config_exists = [bool]$snapshot.exists
        whole_config_sha256 = $snapshot.sha256
        managed_fingerprint_sha256 = $state.managed_fingerprint_sha256
        changed_managed_fields = @($patch.changed_managed_fields)
        production_canary_enabled = [bool]$desired.canary.enabled
        raw_credential_values_recorded = $false
        write_performed = $false
    }
}

function Restore-SmNewestDisplacedSnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)]$DisplacedSnapshot,
        [Parameter(Mandatory=$true)]$ExpectedCurrentSnapshot,
        [Parameter(Mandatory=$true)][string]$TransactionId,
        [int]$Depth = 0,
        [switch]$DelayAfterSourceMove
    )
    if ($Depth -ge 8) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = (Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing).sha256
            failed_candidate_path = [string]$DisplacedSnapshot.path
            failed_candidate_removed = $false
        }
    }
    $current = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
    if (-not (Test-SmConfigSnapshotsEqual -Left $ExpectedCurrentSnapshot -Right $current)) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = [string]$DisplacedSnapshot.path
            failed_candidate_removed = $false
        }
    }
    $source = Assert-SmNoReparsePath -Path ([string]$DisplacedSnapshot.path)
    $artifactIntents = Get-SmRecoveryArtifactIntentPaths `
        -ConfigPath $ConfigPath `
        -TransactionId $TransactionId
    $backup = [string]$artifactIntents.compensation[$Depth]
    if (Test-Path -LiteralPath $backup) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = $backup
            failed_candidate_removed = $false
        }
    }
    $sourceSnapshot = Get-SmConfigByteSnapshot -Path $source
    if (-not (Test-SmConfigSnapshotsEqual -Left $DisplacedSnapshot -Right $sourceSnapshot)) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = [string]$DisplacedSnapshot.path
            failed_candidate_removed = $false
        }
    }
    try {
        [System.IO.File]::Move($ConfigPath, $backup)
    } catch {
        $active = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $active.sha256
            failed_candidate_path = [string]$DisplacedSnapshot.path
            failed_candidate_removed = $false
        }
    }
    $backupSnapshot = Get-SmConfigByteSnapshot -Path $backup
    if (-not (Test-SmConfigSnapshotsEqual -Left $ExpectedCurrentSnapshot -Right $backupSnapshot)) {
        try {
            [System.IO.File]::Move($backup, $ConfigPath)
            $active = Get-SmConfigByteSnapshot -Path $ConfigPath
        } catch {
            $active = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
        }
        return [pscustomobject]@{
            status = $(if (
                Test-SmConfigSnapshotsEqual -Left $backupSnapshot -Right $active
            ) { 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' } else {
                'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            })
            restored_sha256 = $active.sha256
            failed_candidate_path = $(if (Test-Path -LiteralPath $backup) { $backup } else { $null })
            failed_candidate_removed = -not (Test-Path -LiteralPath $backup)
        }
    }
    try {
        [System.IO.File]::Move($source, $ConfigPath)
    } catch {
        $active = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
        if (-not $active.exists) {
            try {
                [System.IO.File]::Move($backup, $ConfigPath)
                $active = Get-SmConfigByteSnapshot -Path $ConfigPath
            } catch {}
        }
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $active.sha256
            failed_candidate_path = $(if (Test-Path -LiteralPath $backup) {
                $backup
            } else {
                [string]$DisplacedSnapshot.path
            })
            failed_candidate_removed = $false
        }
    }
    if ($DelayAfterSourceMove) { Start-Sleep -Milliseconds 1500 }
    $active = Get-SmConfigByteSnapshot -Path $ConfigPath
    if (-not (Test-SmConfigSnapshotsEqual -Left $DisplacedSnapshot -Right $active)) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $active.sha256
            failed_candidate_path = $backup
            failed_candidate_removed = $false
        }
    }
    [void](Remove-SmOwnedSensitiveFile `
        -Path $backup `
        -ExpectedSnapshot $backupSnapshot)
    return [pscustomobject]@{
        status = 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE'
        restored_sha256 = $active.sha256
        failed_candidate_path = $null
        failed_candidate_removed = $true
    }
}

function Restore-SmManagedConfigSnapshot {
    param(
        [Parameter(Mandatory=$true)]$BeforeSnapshot,
        [Parameter(Mandatory=$true)][string]$CandidateSha256,
        [AllowEmptyString()][string]$SwapBackupPath,
        [AllowEmptyString()][string]$PermanentBackupPath,
        $ExpectedCandidateSnapshot,
        [Parameter(Mandatory=$true)][string]$TransactionId,
        [switch]$DelayAfterCas
    )
    $configPath = [string]$BeforeSnapshot.path
    $current = Get-SmConfigByteSnapshot -Path $configPath -AllowMissing
    $candidateCurrent = $current.exists -and $current.sha256 -ceq $CandidateSha256
    if ($candidateCurrent -and $null -ne $ExpectedCandidateSnapshot) {
        $candidateCurrent = Test-SmConfigSnapshotsEqual `
            -Left $ExpectedCandidateSnapshot `
            -Right $current
    }
    if (-not $candidateCurrent) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = $null
            failed_candidate_removed = $false
        }
    }
    if ($DelayAfterCas) { Start-Sleep -Milliseconds 1500 }

    $parent = Split-Path -Parent $configPath
    $artifactIntents = Get-SmRecoveryArtifactIntentPaths `
        -ConfigPath $configPath `
        -TransactionId $TransactionId
    $failedCandidate = [string]$artifactIntents.failed_candidate
    if (Test-Path -LiteralPath $failedCandidate) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = $failedCandidate
            failed_candidate_removed = $false
        }
    }

    if ([bool]$BeforeSnapshot.exists) {
        $rollbackSource = $null
        $rollbackSourceCreated = $false
        if ($SwapBackupPath -and
            (Test-Path -LiteralPath $SwapBackupPath -PathType Leaf)) {
            $swapSourceSnapshot = Get-SmConfigByteSnapshot -Path $SwapBackupPath
            if (Test-SmConfigSnapshotsEqual -Left $BeforeSnapshot -Right $swapSourceSnapshot) {
                $rollbackSource = Assert-SmNoReparsePath -Path $SwapBackupPath
            }
        }
        if (-not $rollbackSource) {
            $permanentSourceSnapshot = $null
            if ($PermanentBackupPath -and
                (Test-Path -LiteralPath $PermanentBackupPath -PathType Leaf)) {
                $permanentSourceSnapshot = Get-SmConfigByteSnapshot -Path $PermanentBackupPath
            }
            if ($null -ne $permanentSourceSnapshot -and
                (Test-SmConfigSnapshotsEqual -Left $BeforeSnapshot -Right $permanentSourceSnapshot)) {
                $originalBytes = [byte[]]$permanentSourceSnapshot.bytes
            } elseif ($BeforeSnapshot.PSObject.Properties.Name -contains 'bytes') {
                $originalBytes = [byte[]]$BeforeSnapshot.bytes
            } else {
                throw 'CONFIG_ROLLBACK_BACKUP_INVALID'
            }
            if ((Get-SmSha256Bytes -Bytes $originalBytes) -cne [string]$BeforeSnapshot.sha256) {
                throw 'CONFIG_ROLLBACK_BACKUP_INVALID'
            }
            $rollbackSource = [string]$artifactIntents.rollback_source
            if (Test-Path -LiteralPath $rollbackSource) {
                return [pscustomobject]@{
                    status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
                    restored_sha256 = $current.sha256
                    failed_candidate_path = $rollbackSource
                    failed_candidate_removed = $false
                }
            }
            Write-SmBytesCreateNew -Path $rollbackSource -Bytes $originalBytes
            try {
                Set-SmFileMetadataFromSnapshot -Path $rollbackSource -Snapshot $BeforeSnapshot
            } catch {
                [void](Remove-SmOwnedSensitiveFile `
                    -Path $rollbackSource `
                    -ExpectedSha256 ([string]$BeforeSnapshot.sha256))
                throw
            }
            $rollbackSourceCreated = $true
        }

        $cas = Get-SmConfigByteSnapshot -Path $configPath -AllowMissing
        $candidateCas = $cas.exists -and $cas.sha256 -ceq $CandidateSha256
        if ($candidateCas -and $null -ne $ExpectedCandidateSnapshot) {
            $candidateCas = Test-SmConfigSnapshotsEqual `
                -Left $ExpectedCandidateSnapshot `
                -Right $cas
        }
        if (-not $candidateCas) {
            if ($rollbackSourceCreated) {
                [void](Remove-SmOwnedSensitiveFile `
                    -Path $rollbackSource `
                    -ExpectedSha256 ([string]$BeforeSnapshot.sha256))
            }
            return [pscustomobject]@{
                status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
                restored_sha256 = $cas.sha256
                failed_candidate_path = $null
                failed_candidate_removed = $false
            }
        }
        $rollbackSourceSnapshot = Get-SmConfigByteSnapshot -Path $rollbackSource
        if (-not (Test-SmConfigSnapshotsEqual -Left $BeforeSnapshot -Right $rollbackSourceSnapshot)) {
            return [pscustomobject]@{
                status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
                restored_sha256 = $cas.sha256
                failed_candidate_path = $rollbackSource
                failed_candidate_removed = $false
            }
        }
        $rollbackResult = Restore-SmNewestDisplacedSnapshot `
            -ConfigPath $configPath `
            -DisplacedSnapshot $rollbackSourceSnapshot `
            -ExpectedCurrentSnapshot $cas `
            -TransactionId $TransactionId
        if ($rollbackResult.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE') {
            $rollbackResult.status = 'ROLLED_BACK_VERIFIED'
        }
        return $rollbackResult
    }

    $cas = Get-SmConfigByteSnapshot -Path $configPath -AllowMissing
    $candidateCas = $cas.exists -and $cas.sha256 -ceq $CandidateSha256
    if ($candidateCas -and $null -ne $ExpectedCandidateSnapshot) {
        $candidateCas = Test-SmConfigSnapshotsEqual `
            -Left $ExpectedCandidateSnapshot `
            -Right $cas
    }
    if (-not $candidateCas) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $cas.sha256
            failed_candidate_path = $null
            failed_candidate_removed = $false
        }
    }
    [System.IO.File]::Move($configPath, $failedCandidate)
    $failedSnapshot = Get-SmConfigByteSnapshot -Path $failedCandidate
    $restored = Get-SmConfigByteSnapshot -Path $configPath -AllowMissing
    if ($restored.exists) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $restored.sha256
            failed_candidate_path = $failedCandidate
            failed_candidate_removed = $false
        }
    }
    if (-not (Test-SmExpectedCandidateSnapshot `
        -Snapshot $failedSnapshot `
        -CandidateSha256 $CandidateSha256 `
        -ExpectedCandidateSnapshot $ExpectedCandidateSnapshot)) {
        try {
            [System.IO.File]::Move($failedCandidate, $configPath)
        } catch {
            $active = Get-SmConfigByteSnapshot -Path $configPath -AllowMissing
            return [pscustomobject]@{
                status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
                restored_sha256 = $active.sha256
                failed_candidate_path = $failedCandidate
                failed_candidate_removed = $false
            }
        }
        $active = Get-SmConfigByteSnapshot -Path $configPath
        if (-not (Test-SmConfigSnapshotsEqual -Left $failedSnapshot -Right $active)) {
            return [pscustomobject]@{
                status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
                restored_sha256 = $active.sha256
                failed_candidate_path = $null
                failed_candidate_removed = $true
            }
        }
        return [pscustomobject]@{
            status = 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE'
            restored_sha256 = $active.sha256
            failed_candidate_path = $null
            failed_candidate_removed = $true
        }
    }
    [void](Remove-SmOwnedSensitiveFile `
        -Path $failedCandidate `
        -ExpectedSnapshot $failedSnapshot)
    return [pscustomobject]@{
        status = 'ROLLED_BACK_VERIFIED'
        restored_sha256 = $null
        failed_candidate_path = $null
        failed_candidate_removed = $true
    }
}

function Restore-SmDisplacedConfigSnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)]$DisplacedSnapshot,
        [Parameter(Mandatory=$true)][string]$CandidateSha256,
        $ExpectedCandidateSnapshot,
        [Parameter(Mandatory=$true)][string]$TransactionId,
        [switch]$DelayAfterCas
    )
    $current = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
    $candidateCurrent = $current.exists -and $current.sha256 -ceq $CandidateSha256
    if ($candidateCurrent -and $null -ne $ExpectedCandidateSnapshot) {
        $candidateCurrent = Test-SmConfigSnapshotsEqual `
            -Left $ExpectedCandidateSnapshot `
            -Right $current
    }
    if (-not $candidateCurrent) {
        return [pscustomobject]@{
            status = 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE'
            restored_sha256 = $current.sha256
            failed_candidate_path = $null
            failed_candidate_removed = $false
        }
    }
    return Restore-SmNewestDisplacedSnapshot `
        -ConfigPath $ConfigPath `
        -DisplacedSnapshot $DisplacedSnapshot `
        -ExpectedCurrentSnapshot $current `
        -TransactionId $TransactionId `
        -DelayAfterSourceMove:$DelayAfterCas
}

function Invoke-SmManagedConfigRepair {
    param(
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [AllowEmptyString()][string]$ExpectedConfigSha256,
        [AllowEmptyString()][string]$BackupRoot,
        [switch]$EnableProductionCanary,
        [AllowEmptyString()][string]$CanaryAuthManifestPath,
        [AllowEmptyString()][string]$CanaryAuthSha256,
        [ValidateSet('none','after_backup','after_temp','after_cas_delay','after_cas_and_restore_delay','after_swap_delay','after_replace','before_verify')]
        [string]$FaultInjection = 'none'
    )
    $desired = Get-SmDesiredManagedConfigState `
        -InstallRoot $InstallRoot `
        -EnableProductionCanary:$EnableProductionCanary `
        -CanaryAuthManifestPath $CanaryAuthManifestPath `
        -CanaryAuthSha256 $CanaryAuthSha256
    $snapshot = Get-SmConfigByteSnapshot -Path $ConfigPath -AllowMissing
    $patch = New-SmManagedConfigPatch -Snapshot $snapshot -DesiredState $desired
    if (-not (Test-SmConfigSnapshotExpectation -Snapshot $snapshot -ExpectedConfigSha256 $ExpectedConfigSha256)) {
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-transaction-result/v2'
            status = 'CAS_CONFLICT_PRECONDITION'
            config_path = $snapshot.path
            whole_config_sha256_before = $snapshot.sha256
            whole_config_sha256_candidate = $patch.candidate_sha256
            transaction_id = $null
            transaction_path = $null
            backup_path = $null
            write_performed = $false
            rollback_performed = $false
            raw_credential_values_recorded = $false
        }
    }
    if ($patch.replay) {
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-transaction-result/v2'
            status = 'REPLAYED_ZERO_WRITE'
            config_path = $snapshot.path
            whole_config_sha256_before = $snapshot.sha256
            whole_config_sha256_candidate = $patch.candidate_sha256
            managed_fingerprint_sha256 = $patch.candidate_managed_fingerprint_sha256
            changed_managed_fields = @()
            production_canary_enabled = [bool]$desired.canary.enabled
            transaction_id = $null
            transaction_path = $null
            backup_path = $null
            write_performed = $false
            rollback_performed = $false
            unmanaged_bytes_preserved = $true
            raw_credential_values_recorded = $false
        }
    }

    $configParent = Assert-SmNoReparsePath -Path (Split-Path -Parent $snapshot.path)
    if (-not (Test-Path -LiteralPath $configParent -PathType Container)) {
        throw 'CONFIG_WRITE_PARENT_MISSING'
    }
    $installFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($InstallRoot))
    if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
        $BackupRoot = Join-Path $installFull 'backups\codex-config'
    }
    $backupFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($BackupRoot)) -AllowMissingLeaf
    if (-not (Test-SmFullPathWithin -Path $backupFull -Root $installFull)) {
        throw 'CONFIG_BACKUP_ROOT_OUTSIDE_INSTALL_ROOT'
    }
    if (-not (Test-Path -LiteralPath $backupFull)) {
        New-Item -ItemType Directory -Path $backupFull | Out-Null
    }
    [void](Assert-SmNoReparsePath -Path $backupFull)

    $transactionId = (
        [DateTimeOffset]::Now.ToString('yyyyMMdd_HHmmssfff') + '-' +
        [guid]::NewGuid().ToString('N').Substring(0, 12)
    )
    $transactionRoot = Join-Path $backupFull ("config-repair__$transactionId")
    New-Item -ItemType Directory -Path $transactionRoot | Out-Null
    [void](Assert-SmNoReparsePath -Path $transactionRoot)
    $journalPath = Join-Path $transactionRoot 'transaction.json'
    $permanentBackup = $(if ($snapshot.exists) {
        Join-Path $transactionRoot 'config.toml.before.preserved'
    } else { $null })
    $temporary = Join-Path $configParent (
        '.{0}.semantic-memory.{1}.candidate.tmp' -f
        [System.IO.Path]::GetFileName($snapshot.path),
        $transactionId
    )
    $swapBackup = $(if ($snapshot.exists) {
        Join-Path $configParent (
            '.{0}.semantic-memory.{1}.swap-backup' -f
            [System.IO.Path]::GetFileName($snapshot.path),
            $transactionId
        )
    } else { $null })
    $artifactIntents = Get-SmRecoveryArtifactIntentPaths `
        -ConfigPath $snapshot.path `
        -TransactionId $transactionId

    $record = [ordered]@{
        schema = 'stage14-config-transaction/v2'
        transaction_id = $transactionId
        created_at = [DateTimeOffset]::Now.ToString('o')
        phase = 'INITIALIZED'
        status = 'IN_PROGRESS'
        config_path = $snapshot.path
        before_exists = [bool]$snapshot.exists
        before_sha256 = $snapshot.sha256
        candidate_sha256 = $patch.candidate_sha256
        candidate_acl_sddl = $null
        candidate_attributes = $null
        candidate_managed_fingerprint_sha256 = $patch.candidate_managed_fingerprint_sha256
        changed_managed_fields = @($patch.changed_managed_fields)
        production_canary = $desired.canary
        unmanaged_projection_sha256_before = $patch.unmanaged_projection_sha256_before
        unmanaged_projection_sha256_after = $patch.unmanaged_projection_sha256_after
        unmanaged_bytes_preserved = [bool]$patch.unmanaged_bytes_preserved
        permanent_backup_path = $permanentBackup
        swap_backup_path = $swapBackup
        temporary_path = $temporary
        artifact_intent_paths = @($artifactIntents.all)
        failure_candidate_path = $null
        displaced_external_sha256 = $null
        displaced_external_acl_sddl = $null
        displaced_external_attributes = $null
        temporary_removed = $false
        swap_backup_removed = $false
        failed_candidate_removed = $false
        recovery_observed_exists = $null
        recovery_observed_sha256 = $null
        recovery_observed_acl_sddl = $null
        recovery_observed_attributes = $null
        recovery_artifacts_removed = @()
        recovery_artifact_paths_present = @()
        error_code = $null
        raw_credential_values_recorded = $false
    }

    $swapAttempted = $false
    $swapped = $false
    $displacedSnapshot = $null
    $candidateAppliedSnapshot = $null
    try {
        # Persist every deterministic sensitive-artifact intent before any
        # configuration bytes are copied outside the active config path.
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($snapshot.exists) {
            Write-SmBytesCreateNew -Path $permanentBackup -Bytes ([byte[]]$snapshot.bytes)
            Set-SmFileMetadataFromSnapshot -Path $permanentBackup -Snapshot $snapshot
            $permanentSnapshot = Get-SmConfigByteSnapshot -Path $permanentBackup
            if (-not (Test-SmConfigSnapshotsEqual -Left $snapshot -Right $permanentSnapshot)) {
                throw 'CONFIG_PERMANENT_BACKUP_VERIFY_FAILED'
            }
        }
        $record['phase'] = 'BACKUP_VERIFIED'
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($FaultInjection -ceq 'after_backup') { throw 'FAULT_INJECTION_AFTER_BACKUP' }

        Write-SmBytesCreateNew -Path $temporary -Bytes ([byte[]]$patch.candidate_bytes)
        if ($snapshot.exists) {
            Set-SmFileMetadataFromSnapshot -Path $temporary -Snapshot $snapshot
        }
        if ((Get-SmSha256File -Path $temporary) -cne [string]$patch.candidate_sha256) {
            throw 'CONFIG_CANDIDATE_TEMP_VERIFY_FAILED'
        }
        $candidateTemporarySnapshot = Get-SmConfigByteSnapshot -Path $temporary
        $candidateExpectedSnapshot = $(if ($snapshot.exists) {
            [pscustomobject]@{
                exists = $true
                sha256 = [string]$patch.candidate_sha256
                acl_sddl = $snapshot.acl_sddl
                attributes = $snapshot.attributes
            }
        } else {
            $candidateTemporarySnapshot
        })
        $record['candidate_acl_sddl'] = $candidateExpectedSnapshot.acl_sddl
        $record['candidate_attributes'] = [int]$candidateExpectedSnapshot.attributes
        $record['phase'] = 'CANDIDATE_TEMP_VERIFIED'
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($FaultInjection -ceq 'after_temp') { throw 'FAULT_INJECTION_AFTER_TEMP' }

        $cas = Get-SmConfigByteSnapshot -Path $snapshot.path -AllowMissing
        if (-not (Test-SmConfigSnapshotsEqual -Left $snapshot -Right $cas)) {
            throw 'CONFIG_CAS_CONFLICT'
        }
        $record['phase'] = 'CAS_VERIFIED'
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($FaultInjection -in @('after_cas_delay','after_cas_and_restore_delay')) {
            Start-Sleep -Milliseconds 1500
        }

        $swapAttempted = $true
        if ($snapshot.exists) {
            [System.IO.File]::Replace($temporary, $snapshot.path, $swapBackup, $false)
        } else {
            [System.IO.File]::Move($temporary, $snapshot.path)
        }
        $swapped = $true
        if ($FaultInjection -ceq 'after_swap_delay') {
            Start-Sleep -Milliseconds 1500
        }
        $candidateAppliedSnapshot = Get-SmConfigByteSnapshot -Path $snapshot.path
        if (-not (Test-SmConfigSnapshotsEqual `
            -Left $candidateExpectedSnapshot `
            -Right $candidateAppliedSnapshot)) {
            throw 'CONFIG_POST_SWAP_SNAPSHOT_MISMATCH'
        }
        if ($snapshot.exists) {
            if (-not (Test-Path -LiteralPath $swapBackup -PathType Leaf)) {
                throw 'CONFIG_SWAP_BACKUP_MISSING'
            }
            $swapSnapshot = Get-SmConfigByteSnapshot -Path $swapBackup
            if (-not (Test-SmConfigSnapshotsEqual -Left $snapshot -Right $swapSnapshot)) {
                $displacedSnapshot = $swapSnapshot
                $record['displaced_external_sha256'] = $swapSnapshot.sha256
                $record['displaced_external_acl_sddl'] = $swapSnapshot.acl_sddl
                $record['displaced_external_attributes'] = $swapSnapshot.attributes
                $record['phase'] = 'DISPLACED_EXTERNAL_DETECTED'
                try { Write-SmAtomicJsonSafe -Path $journalPath -Value $record } catch {}
                throw 'CONFIG_CAS_CONFLICT_DISPLACED_EXTERNAL_CHANGE'
            }
        }
        $record['phase'] = 'SWAPPED'
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($FaultInjection -ceq 'after_replace') { throw 'FAULT_INJECTION_AFTER_REPLACE' }
        if ($FaultInjection -ceq 'before_verify') { throw 'FAULT_INJECTION_BEFORE_VERIFY' }

        $verified = Get-SmConfigByteSnapshot -Path $snapshot.path
        if ($verified.sha256 -cne [string]$patch.candidate_sha256) {
            throw 'CONFIG_POST_WRITE_SHA256_MISMATCH'
        }
        if (-not (Test-SmManagedSnapshotMatchesDesired -Snapshot $verified -DesiredState $desired)) {
            throw 'CONFIG_POST_WRITE_MANAGED_VERIFY_FAILED'
        }
        if ($snapshot.exists -and
            [string]$snapshot.acl_sddl -cne [string]$verified.acl_sddl) {
            throw 'CONFIG_POST_WRITE_ACL_MISMATCH'
        }
        if ($snapshot.exists -and
            [int]$snapshot.attributes -ne [int]$verified.attributes) {
            throw 'CONFIG_POST_WRITE_ATTRIBUTES_MISMATCH'
        }
        $record['phase'] = 'COMMITTED'
        $record['status'] = 'APPLIED_VERIFIED'
        Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        if ($snapshot.exists) {
            $record['swap_backup_removed'] = [bool](
                Remove-SmOwnedSensitiveFile `
                    -Path $swapBackup `
                    -ExpectedSha256 ([string]$snapshot.sha256)
            )
            Write-SmAtomicJsonSafe -Path $journalPath -Value $record
        }
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-transaction-result/v2'
            status = 'APPLIED_VERIFIED'
            config_path = $snapshot.path
            whole_config_sha256_before = $snapshot.sha256
            whole_config_sha256_after = $verified.sha256
            whole_config_sha256_candidate = $patch.candidate_sha256
            managed_fingerprint_sha256 = $patch.candidate_managed_fingerprint_sha256
            changed_managed_fields = @($patch.changed_managed_fields)
            production_canary_enabled = [bool]$desired.canary.enabled
            transaction_id = $transactionId
            transaction_path = $journalPath
            backup_path = $permanentBackup
            swap_backup_path = $swapBackup
            swap_backup_removed = [bool]$record['swap_backup_removed']
            write_performed = $true
            rollback_performed = $false
            unmanaged_bytes_preserved = [bool]$patch.unmanaged_bytes_preserved
            acl_preserved = $(if ($snapshot.exists) { $snapshot.acl_sddl -ceq $verified.acl_sddl } else { $true })
            attributes_preserved = $(if ($snapshot.exists) { [int]$snapshot.attributes -eq [int]$verified.attributes } else { $true })
            raw_credential_values_recorded = $false
        }
    } catch {
        $errorCode = Get-SmSafeConfigErrorCode -Message ([string]$_.Exception.Message)
        if ($swapAttempted -and -not $swapped) {
            try {
                $probe = Get-SmConfigByteSnapshot -Path $snapshot.path -AllowMissing
                if ($null -ne $candidateExpectedSnapshot -and
                    (Test-SmConfigSnapshotsEqual `
                        -Left $candidateExpectedSnapshot `
                        -Right $probe)) {
                    $swapped = $true
                    $candidateAppliedSnapshot = $probe
                }
            } catch {}
        }
        $rollback = $null
        if ($swapped) {
            try {
                if ($null -ne $displacedSnapshot) {
                    $rollback = Restore-SmDisplacedConfigSnapshot `
                        -ConfigPath $snapshot.path `
                        -DisplacedSnapshot $displacedSnapshot `
                        -CandidateSha256 $patch.candidate_sha256 `
                        -ExpectedCandidateSnapshot $candidateExpectedSnapshot `
                        -TransactionId $transactionId `
                        -DelayAfterCas:($FaultInjection -ceq 'after_cas_and_restore_delay')
                } else {
                    $rollback = Restore-SmManagedConfigSnapshot `
                        -BeforeSnapshot $snapshot `
                        -CandidateSha256 $patch.candidate_sha256 `
                        -SwapBackupPath $swapBackup `
                        -PermanentBackupPath $permanentBackup `
                        -ExpectedCandidateSnapshot $candidateExpectedSnapshot `
                        -TransactionId $transactionId `
                        -DelayAfterCas:($FaultInjection -ceq 'after_cas_and_restore_delay')
                }
            } catch {
                $rollback = [pscustomobject]@{
                    status = 'ROLLBACK_FAILED'
                    restored_sha256 = $null
                    failed_candidate_path = $null
                    failed_candidate_removed = $false
                }
            }
        }
        if (-not $swapped) {
            try {
                $record['temporary_removed'] = [bool](
                    Remove-SmOwnedSensitiveFile `
                        -Path $temporary `
                        -ExpectedSha256 ([string]$patch.candidate_sha256)
                )
            } catch {
                $errorCode = 'CONFIG_SENSITIVE_TEMP_CLEANUP_FAILED'
            }
        }
        $finalStatus = $(if (-not $swapped) { 'FAILED_BEFORE_SWAP' }
            elseif ($rollback.status -ceq 'ROLLED_BACK_VERIFIED') { 'ROLLED_BACK_VERIFIED' }
            elseif ($rollback.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE') { 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' }
            elseif ($rollback.status -ceq 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE') { 'ROLLBACK_CONFLICT_EXTERNAL_CHANGE' }
            else { 'ROLLBACK_FAILED' })
        if ($rollback -and
            $rollback.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE') {
            $rollbackTerminalSnapshot = Get-SmConfigByteSnapshot `
                -Path $snapshot.path `
                -AllowMissing
            $record['displaced_external_sha256'] = $rollbackTerminalSnapshot.sha256
            $record['displaced_external_acl_sddl'] = $rollbackTerminalSnapshot.acl_sddl
            $record['displaced_external_attributes'] = $rollbackTerminalSnapshot.attributes
        }
        $record['phase'] = $finalStatus
        $record['status'] = $finalStatus
        $record['error_code'] = $errorCode
        if ($rollback) {
            $record['failure_candidate_path'] = $rollback.failed_candidate_path
            $record['failed_candidate_removed'] = [bool]$rollback.failed_candidate_removed
        }
        try { Write-SmAtomicJsonSafe -Path $journalPath -Value $record } catch {}
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-transaction-result/v2'
            status = $finalStatus
            error_code = $errorCode
            config_path = $snapshot.path
            whole_config_sha256_before = $snapshot.sha256
            whole_config_sha256_candidate = $patch.candidate_sha256
            whole_config_sha256_after = $(if ($rollback) { $rollback.restored_sha256 } else { $snapshot.sha256 })
            changed_managed_fields = @($patch.changed_managed_fields)
            transaction_id = $transactionId
            transaction_path = $journalPath
            backup_path = $permanentBackup
            swap_backup_path = $swapBackup
            failure_candidate_path = $(if ($rollback) { $rollback.failed_candidate_path } else { $null })
            temporary_removed = [bool]$record['temporary_removed']
            failed_candidate_removed = $(if ($rollback) { [bool]$rollback.failed_candidate_removed } else { $false })
            write_performed = $swapped
            rollback_performed = (
                $rollback -and
                $rollback.status -in @('ROLLED_BACK_VERIFIED','CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE')
            )
            raw_credential_values_recorded = $false
        }
    }
}

function Recover-SmManagedConfigTransaction {
    param(
        [Parameter(Mandatory=$true)][string]$TransactionPath,
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [AllowEmptyString()][string]$BackupRoot
    )
    $installFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($InstallRoot))
    if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
        $BackupRoot = Join-Path $installFull 'backups\codex-config'
    }
    $backupFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($BackupRoot))
    $transactionFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($TransactionPath))
    if (-not (Test-SmFullPathWithin -Path $backupFull -Root $installFull) -or
        -not (Test-SmFullPathWithin -Path $transactionFull -Root $backupFull)) {
        throw 'CONFIG_RECOVERY_PATH_OUTSIDE_TRUSTED_ROOT'
    }
    $record = Read-SmJson -Path $transactionFull
    if ($record.schema -cne 'stage14-config-transaction/v2' -or
        [string]$record.transaction_id -notmatch '^[0-9]{8}_[0-9]{9}-[0-9a-f]{12}$' -or
        [string]$record.candidate_sha256 -notmatch '^[0-9a-f]{64}$' -or
        ([bool]$record.before_exists -and [string]$record.before_sha256 -notmatch '^[0-9a-f]{64}$')) {
        throw 'CONFIG_RECOVERY_JOURNAL_INVALID'
    }
    $configFull = Assert-SmNoReparsePath -Path ([System.IO.Path]::GetFullPath($ConfigPath)) -AllowMissingLeaf
    if (-not [string]::Equals(
        $configFull,
        [System.IO.Path]::GetFullPath([string]$record.config_path),
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'CONFIG_RECOVERY_CONFIG_PATH_MISMATCH'
    }

    $transactionRoot = Split-Path -Parent $transactionFull
    $expectedTransactionRoot = Join-Path $backupFull (
        'config-repair__' + [string]$record.transaction_id
    )
    $expectedTransactionPath = Join-Path $expectedTransactionRoot 'transaction.json'
    if (-not [string]::Equals(
        $transactionRoot,
        $expectedTransactionRoot,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or -not [string]::Equals(
        $transactionFull,
        $expectedTransactionPath,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'CONFIG_RECOVERY_TRANSACTION_PATH_MISMATCH'
    }
    $configParent = Split-Path -Parent $configFull
    $configFileName = [System.IO.Path]::GetFileName($configFull)
    $expectedTemporary = Join-Path $configParent (
        '.{0}.semantic-memory.{1}.candidate.tmp' -f
        $configFileName,
        [string]$record.transaction_id
    )
    $expectedSwapBackup = $(if ([bool]$record.before_exists) {
        Join-Path $configParent (
            '.{0}.semantic-memory.{1}.swap-backup' -f
            $configFileName,
            [string]$record.transaction_id
        )
    } else { $null })
    $expectedPermanentBackup = $(if ([bool]$record.before_exists) {
        Join-Path $transactionRoot 'config.toml.before.preserved'
    } else { $null })
    $expectedArtifactIntents = Get-SmRecoveryArtifactIntentPaths `
        -ConfigPath $configFull `
        -TransactionId ([string]$record.transaction_id)
    $recordArtifactIntents = @($record.artifact_intent_paths)
    if ($recordArtifactIntents.Count -ne @($expectedArtifactIntents.all).Count) {
        throw 'CONFIG_RECOVERY_ARTIFACT_INTENTS_INVALID'
    }
    for ($artifactIndex = 0; $artifactIndex -lt $recordArtifactIntents.Count; $artifactIndex++) {
        $recordArtifactPath = [string]$recordArtifactIntents[$artifactIndex]
        $expectedArtifactPath = [string]$expectedArtifactIntents.all[$artifactIndex]
        if ([string]::IsNullOrWhiteSpace($recordArtifactPath) -or
            -not [string]::Equals(
                [System.IO.Path]::GetFullPath($recordArtifactPath),
                $expectedArtifactPath,
                [System.StringComparison]::OrdinalIgnoreCase
            ) -or
            [string]::Equals(
                [System.IO.Path]::GetFullPath($recordArtifactPath),
                $configFull,
                [System.StringComparison]::OrdinalIgnoreCase
            ) -or
            -not (Test-SmFullPathWithin -Path $recordArtifactPath -Root $configParent)) {
            throw 'CONFIG_RECOVERY_ARTIFACT_INTENTS_INVALID'
        }
    }
    $permanentBackup = [string]$record.permanent_backup_path
    $temporary = [string]$record.temporary_path
    $swapBackup = [string]$record.swap_backup_path
    if ([string]::IsNullOrWhiteSpace($temporary) -or -not [string]::Equals(
        [System.IO.Path]::GetFullPath($temporary),
        $expectedTemporary,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or [string]::Equals(
        [System.IO.Path]::GetFullPath($temporary),
        $configFull,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'CONFIG_RECOVERY_TEMP_PATH_INVALID'
    }
    if ([bool]$record.before_exists) {
        if ([string]::IsNullOrWhiteSpace($swapBackup) -or
            [string]::IsNullOrWhiteSpace($permanentBackup) -or
            -not [string]::Equals(
            [System.IO.Path]::GetFullPath($swapBackup),
            $expectedSwapBackup,
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or [string]::Equals(
            [System.IO.Path]::GetFullPath($swapBackup),
            $configFull,
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or -not [string]::Equals(
            [System.IO.Path]::GetFullPath($permanentBackup),
            $expectedPermanentBackup,
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or [string]::Equals(
            [System.IO.Path]::GetFullPath($permanentBackup),
            $configFull,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw 'CONFIG_RECOVERY_BACKUP_PATH_INVALID'
        }
    } elseif ($swapBackup -or $permanentBackup) {
        throw 'CONFIG_RECOVERY_UNEXPECTED_BACKUP_PATH'
    }
    $beforeSnapshot = [pscustomobject]@{
        path = $configFull
        exists = [bool]$record.before_exists
        sha256 = $record.before_sha256
        acl_sddl = $null
        attributes = $null
    }
    if ([bool]$record.before_exists) {
        if (-not $permanentBackup -or
            -not (Test-SmFullPathWithin -Path $permanentBackup -Root $transactionRoot) -or
            -not (Test-Path -LiteralPath $permanentBackup -PathType Leaf)) {
            throw 'CONFIG_RECOVERY_BACKUP_INVALID'
        }
        $permanentSnapshot = Get-SmConfigByteSnapshot `
            -Path (Assert-SmNoReparsePath -Path $permanentBackup)
        if ($permanentSnapshot.sha256 -cne [string]$record.before_sha256) {
            throw 'CONFIG_RECOVERY_BACKUP_INVALID'
        }
        $beforeSnapshot.acl_sddl = $permanentSnapshot.acl_sddl
        $beforeSnapshot.attributes = $permanentSnapshot.attributes
    }
    if ([string]::IsNullOrWhiteSpace([string]$record.candidate_acl_sddl) -or
        $null -eq $record.candidate_attributes) {
        # INITIALIZED/BACKUP_VERIFIED transactions have not reached a swap and
        # are handled by the exact before-state branch below. Any observed
        # candidate requires the metadata witness written before CAS.
        $candidateMetadataWitness = $null
    } else {
        $candidateMetadataWitness = [pscustomobject]@{
            exists = $true
            sha256 = [string]$record.candidate_sha256
            acl_sddl = [string]$record.candidate_acl_sddl
            attributes = [int]$record.candidate_attributes
        }
        if ([bool]$record.before_exists -and
            ([string]$candidateMetadataWitness.acl_sddl -cne [string]$beforeSnapshot.acl_sddl -or
             [int]$candidateMetadataWitness.attributes -ne [int]$beforeSnapshot.attributes)) {
            throw 'CONFIG_RECOVERY_CANDIDATE_METADATA_INVALID'
        }
    }
    if ($swapBackup) {
        if (-not (Test-SmFullPathWithin -Path $swapBackup -Root $configParent)) {
            throw 'CONFIG_RECOVERY_SWAP_PATH_INVALID'
        }
    }
    if ($temporary) {
        if (-not (Test-SmFullPathWithin -Path $temporary -Root $configParent)) {
            throw 'CONFIG_RECOVERY_TEMP_PATH_INVALID'
        }
    }

    $recoveryCanaryEnabled = $false
    $recoveryCanaryAuthPath = $null
    $recoveryCanaryAuthSha256 = $null
    if ($record.PSObject.Properties.Name -contains 'production_canary') {
        $recoveryCanaryEnabled = [bool]$record.production_canary.enabled
        $recoveryCanaryAuthPath = [string]$record.production_canary.auth_manifest_path
        $recoveryCanaryAuthSha256 = [string]$record.production_canary.auth_manifest_sha256
        if ($recoveryCanaryEnabled -and (
            -not [System.IO.Path]::IsPathRooted($recoveryCanaryAuthPath) -or
            $recoveryCanaryAuthSha256 -notmatch '^[0-9a-f]{64}$'
        )) {
            throw 'CONFIG_RECOVERY_CANARY_BINDING_INVALID'
        }
    }
    $current = Get-SmConfigByteSnapshot -Path $configFull -AllowMissing
    $artifactCleanupChanged = $false
    $removedArtifactPaths = New-Object System.Collections.Generic.List[string]
    foreach ($priorRemoved in @($record.recovery_artifacts_removed)) {
        if (-not [string]::IsNullOrWhiteSpace([string]$priorRemoved)) {
            $removedArtifactPaths.Add([string]$priorRemoved)
        }
    }
    $unresolvedArtifactPaths = New-Object System.Collections.Generic.List[string]
    foreach ($artifactPath in @($expectedArtifactIntents.all)) {
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) { continue }
        if (-not $current.exists) {
            $unresolvedArtifactPaths.Add([string]$artifactPath)
            continue
        }
        $artifactSnapshot = Get-SmConfigByteSnapshot -Path $artifactPath
        $safeArtifact = (
            [string]::Equals(
                [string]$artifactPath,
                [string]$expectedArtifactIntents.rollback_source,
                [System.StringComparison]::OrdinalIgnoreCase
            ) -and
            [bool]$beforeSnapshot.exists -and
            (Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $artifactSnapshot)
        )
        if (-not $safeArtifact -and $null -ne $candidateMetadataWitness) {
            $safeArtifact = Test-SmConfigSnapshotsEqual `
                -Left $candidateMetadataWitness `
                -Right $artifactSnapshot
        }
        if ($safeArtifact) {
            [void](Remove-SmOwnedSensitiveFile `
                -Path $artifactPath `
                -ExpectedSnapshot $artifactSnapshot)
            $removedArtifactPaths.Add([string]$artifactPath)
            $artifactCleanupChanged = $true
        } else {
            $unresolvedArtifactPaths.Add([string]$artifactPath)
        }
    }
    $priorPresentArtifactPaths = @($record.recovery_artifact_paths_present)
    $record.recovery_artifacts_removed = @($removedArtifactPaths)
    $record.recovery_artifact_paths_present = @($unresolvedArtifactPaths)
    if ($unresolvedArtifactPaths.Count -gt 0) {
        $artifactConflictSnapshot = $(if (
            $record.PSObject.Properties.Name -contains 'recovery_observed_exists' -and
            $null -ne $record.recovery_observed_exists
        ) {
            [pscustomobject]@{
                exists = [bool]$record.recovery_observed_exists
                sha256 = $record.recovery_observed_sha256
                acl_sddl = $record.recovery_observed_acl_sddl
                attributes = $record.recovery_observed_attributes
            }
        } else { $null })
        $sameArtifactPaths = (
            [string]::Join("`0", @($priorPresentArtifactPaths)) -ceq
            [string]::Join("`0", @($unresolvedArtifactPaths))
        )
        $alreadyArtifactConflict = (
            [string]$record.phase -ceq 'RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT' -and
            [string]$record.status -ceq 'RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT' -and
            $sameArtifactPaths -and
            $null -ne $artifactConflictSnapshot -and
            (Test-SmConfigSnapshotsEqual -Left $artifactConflictSnapshot -Right $current) -and
            -not $artifactCleanupChanged
        )
        if (-not $alreadyArtifactConflict) {
            $record.phase = 'RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT'
            $record.status = 'RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT'
            $record.error_code = 'CONFIG_RECOVERY_SENSITIVE_ARTIFACT_CONFLICT'
            $record.recovery_observed_exists = [bool]$current.exists
            $record.recovery_observed_sha256 = $current.sha256
            $record.recovery_observed_acl_sddl = $current.acl_sddl
            $record.recovery_observed_attributes = $current.attributes
            Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
        }
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-recovery-result/v2'
            status = 'RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT'
            transaction_id = $record.transaction_id
            transaction_path = $transactionFull
            config_path = $configFull
            whole_config_sha256 = $current.sha256
            recovery_artifact_paths_present = @($unresolvedArtifactPaths)
            config_write_performed = $false
            journal_write_performed = (-not $alreadyArtifactConflict)
            raw_credential_values_recorded = $false
        }
    }
    $isBefore = Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $current
    if ($isBefore) {
        $cleanupChanged = $artifactCleanupChanged
        if ($temporary -and (Test-Path -LiteralPath $temporary -PathType Leaf)) {
            $cleanupChanged = [bool](
                Remove-SmOwnedSensitiveFile `
                    -Path $temporary `
                    -ExpectedSha256 ([string]$record.candidate_sha256)
            ) -or $cleanupChanged
        }
        if ($swapBackup -and (Test-Path -LiteralPath $swapBackup -PathType Leaf)) {
            $swapSnapshot = Get-SmConfigByteSnapshot -Path $swapBackup
            if (Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $swapSnapshot) {
                $cleanupChanged = [bool](
                    Remove-SmOwnedSensitiveFile `
                        -Path $swapBackup `
                        -ExpectedSha256 ([string]$record.before_sha256)
                ) -or $cleanupChanged
            }
        }
        $postCleanup = Get-SmConfigByteSnapshot -Path $configFull -AllowMissing
        if (-not (Test-SmConfigSnapshotsEqual -Left $current -Right $postCleanup)) {
            throw 'CONFIG_RECOVERY_CLEANUP_CHANGED_CONFIG'
        }
        $current = $postCleanup
        $alreadyRecovered = (
            [string]$record.phase -ceq 'RECOVERED_ROLLBACK_VERIFIED' -and
            [string]$record.status -ceq 'RECOVERED_ROLLBACK_VERIFIED'
        )
        if (-not $alreadyRecovered -or $cleanupChanged) {
            $record.phase = 'RECOVERED_ROLLBACK_VERIFIED'
            $record.status = 'RECOVERED_ROLLBACK_VERIFIED'
            $record.error_code = $null
            if ($record.PSObject.Properties.Name -contains 'temporary_removed') {
                $record.temporary_removed = $true
            }
            if ($record.PSObject.Properties.Name -contains 'swap_backup_removed' -and
                (-not $swapBackup -or -not (Test-Path -LiteralPath $swapBackup -PathType Leaf))) {
                $record.swap_backup_removed = $true
            }
            Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
        }
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-recovery-result/v2'
            status = 'RECOVERED_ROLLBACK_VERIFIED'
            transaction_id = $record.transaction_id
            transaction_path = $transactionFull
            config_path = $configFull
            whole_config_sha256 = $current.sha256
            config_write_performed = $false
            journal_write_performed = (-not $alreadyRecovered -or $cleanupChanged)
            raw_credential_values_recorded = $false
        }
    }

    $recoveryConflictCode = 'CONFIG_RECOVERY_CAS_CONFLICT'
    $hasCandidateBytes = $current.exists -and
        $current.sha256 -ceq [string]$record.candidate_sha256
    $isCandidate = $hasCandidateBytes -and
        $null -ne $candidateMetadataWitness -and
        (Test-SmConfigSnapshotsEqual -Left $candidateMetadataWitness -Right $current)
    if ($hasCandidateBytes -and -not $isCandidate) {
        $recoveryConflictCode = 'CONFIG_RECOVERY_CANDIDATE_METADATA_MISMATCH'
    }
    $crashSwapSnapshot = $null
    $phaseRequiresSwapProof = (
        [bool]$record.before_exists -and
        [string]$record.phase -in @('CAS_VERIFIED','DISPLACED_EXTERNAL_DETECTED')
    )
    if ($isCandidate -and $phaseRequiresSwapProof) {
        if (-not $swapBackup -or
            -not (Test-Path -LiteralPath $swapBackup -PathType Leaf)) {
            $isCandidate = $false
            $recoveryConflictCode = 'CONFIG_RECOVERY_SWAP_BACKUP_MISSING'
        } else {
            $crashSwapSnapshot = Get-SmConfigByteSnapshot -Path $swapBackup
            if ([string]$record.phase -ceq 'DISPLACED_EXTERNAL_DETECTED') {
                $journalDisplacedSnapshot = $(if (
                    $record.PSObject.Properties.Name -contains 'displaced_external_sha256' -and
                    [string]$record.displaced_external_sha256 -match '^[0-9a-f]{64}$' -and
                    $record.PSObject.Properties.Name -contains 'displaced_external_acl_sddl' -and
                    -not [string]::IsNullOrWhiteSpace([string]$record.displaced_external_acl_sddl) -and
                    $record.PSObject.Properties.Name -contains 'displaced_external_attributes' -and
                    $null -ne $record.displaced_external_attributes
                ) {
                    [pscustomobject]@{
                        exists = $true
                        sha256 = [string]$record.displaced_external_sha256
                        acl_sddl = [string]$record.displaced_external_acl_sddl
                        attributes = [int]$record.displaced_external_attributes
                    }
                } else { $null })
                if ($null -eq $journalDisplacedSnapshot -or
                    -not (Test-SmConfigSnapshotsEqual `
                        -Left $journalDisplacedSnapshot `
                        -Right $crashSwapSnapshot) -or
                    (Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $crashSwapSnapshot)) {
                    $isCandidate = $false
                    $recoveryConflictCode = 'CONFIG_RECOVERY_DISPLACED_BACKUP_INVALID'
                }
            }
        }
    }
    if ($isCandidate) {
        if ($phaseRequiresSwapProof) {
            $swapSnapshot = $crashSwapSnapshot
            if (-not (Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $swapSnapshot)) {
                $rollback = Restore-SmDisplacedConfigSnapshot `
                    -ConfigPath $configFull `
                    -DisplacedSnapshot $swapSnapshot `
                    -CandidateSha256 ([string]$record.candidate_sha256) `
                    -ExpectedCandidateSnapshot $current `
                    -TransactionId ([string]$record.transaction_id)
                $record.phase = $rollback.status
                $record.status = $rollback.status
                $record.failure_candidate_path = $rollback.failed_candidate_path
                if ($rollback.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' -and
                    $record.PSObject.Properties.Name -contains 'displaced_external_sha256') {
                    $recoveredDisplacedSnapshot = Get-SmConfigByteSnapshot `
                        -Path $configFull `
                        -AllowMissing
                    $record.displaced_external_sha256 = $recoveredDisplacedSnapshot.sha256
                    $record.displaced_external_acl_sddl = $recoveredDisplacedSnapshot.acl_sddl
                    $record.displaced_external_attributes = $recoveredDisplacedSnapshot.attributes
                }
                if ($record.PSObject.Properties.Name -contains 'failed_candidate_removed') {
                    $record.failed_candidate_removed = [bool]$rollback.failed_candidate_removed
                }
                Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
                return [pscustomobject][ordered]@{
                    schema = 'stage14-config-recovery-result/v2'
                    status = $rollback.status
                    transaction_id = $record.transaction_id
                    transaction_path = $transactionFull
                    config_path = $configFull
                    whole_config_sha256 = $rollback.restored_sha256
                    failure_candidate_path = $rollback.failed_candidate_path
                    config_write_performed = (
                        $rollback.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE'
                    )
                    journal_write_performed = $true
                    raw_credential_values_recorded = $false
                }
            }
        }

        $managedMatches = $false
        try {
            $desired = Get-SmDesiredManagedConfigState `
                -InstallRoot $installFull `
                -EnableProductionCanary:$recoveryCanaryEnabled `
                -CanaryAuthManifestPath $recoveryCanaryAuthPath `
                -CanaryAuthSha256 $recoveryCanaryAuthSha256
            $managedMatches = Test-SmManagedSnapshotMatchesDesired `
                -Snapshot $current `
                -DesiredState $desired
        } catch {
            $managedMatches = $false
        }
        if ($managedMatches) {
            $cleanupChanged = $artifactCleanupChanged
            if ($temporary -and (Test-Path -LiteralPath $temporary -PathType Leaf)) {
                $cleanupChanged = [bool](
                    Remove-SmOwnedSensitiveFile `
                        -Path $temporary `
                        -ExpectedSha256 ([string]$record.candidate_sha256)
                ) -or $cleanupChanged
            }
            if ($swapBackup -and (Test-Path -LiteralPath $swapBackup -PathType Leaf) -and
                [bool]$record.before_exists) {
                $swapSnapshot = Get-SmConfigByteSnapshot -Path $swapBackup
                if (Test-SmConfigSnapshotsEqual -Left $beforeSnapshot -Right $swapSnapshot) {
                    $cleanupChanged = [bool](
                        Remove-SmOwnedSensitiveFile `
                            -Path $swapBackup `
                            -ExpectedSha256 ([string]$record.before_sha256)
                    ) -or $cleanupChanged
                }
            }
            $postCleanup = Get-SmConfigByteSnapshot -Path $configFull -AllowMissing
            if (-not (Test-SmConfigSnapshotsEqual -Left $current -Right $postCleanup)) {
                throw 'CONFIG_RECOVERY_CLEANUP_CHANGED_CONFIG'
            }
            $current = $postCleanup
            $alreadyRecovered = (
                [string]$record.phase -ceq 'RECOVERED_COMMIT_VERIFIED' -and
                [string]$record.status -ceq 'RECOVERED_COMMIT_VERIFIED'
            )
            if (-not $alreadyRecovered -or $cleanupChanged) {
                $record.phase = 'RECOVERED_COMMIT_VERIFIED'
                $record.status = 'RECOVERED_COMMIT_VERIFIED'
                $record.error_code = $null
                if ($record.PSObject.Properties.Name -contains 'temporary_removed') {
                    $record.temporary_removed = $true
                }
                if ($record.PSObject.Properties.Name -contains 'swap_backup_removed' -and
                    (-not $swapBackup -or -not (Test-Path -LiteralPath $swapBackup -PathType Leaf))) {
                    $record.swap_backup_removed = $true
                }
                Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
            }
            return [pscustomobject][ordered]@{
                schema = 'stage14-config-recovery-result/v2'
                status = 'RECOVERED_COMMIT_VERIFIED'
                transaction_id = $record.transaction_id
                transaction_path = $transactionFull
                config_path = $configFull
                whole_config_sha256 = $current.sha256
                config_write_performed = $false
                journal_write_performed = (-not $alreadyRecovered -or $cleanupChanged)
                raw_credential_values_recorded = $false
            }
        }

        $rollback = Restore-SmManagedConfigSnapshot `
            -BeforeSnapshot $beforeSnapshot `
            -CandidateSha256 ([string]$record.candidate_sha256) `
            -SwapBackupPath $swapBackup `
            -PermanentBackupPath $permanentBackup `
            -ExpectedCandidateSnapshot $current `
            -TransactionId ([string]$record.transaction_id)
        $record.phase = $rollback.status
        $record.status = $rollback.status
        $record.failure_candidate_path = $rollback.failed_candidate_path
        if ($rollback.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' -and
            $record.PSObject.Properties.Name -contains 'displaced_external_sha256') {
            $recoveredDisplacedSnapshot = Get-SmConfigByteSnapshot `
                -Path $configFull `
                -AllowMissing
            $record.displaced_external_sha256 = $recoveredDisplacedSnapshot.sha256
            $record.displaced_external_acl_sddl = $recoveredDisplacedSnapshot.acl_sddl
            $record.displaced_external_attributes = $recoveredDisplacedSnapshot.attributes
        }
        if ($record.PSObject.Properties.Name -contains 'failed_candidate_removed') {
            $record.failed_candidate_removed = [bool]$rollback.failed_candidate_removed
        }
        Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-recovery-result/v2'
            status = $rollback.status
            transaction_id = $record.transaction_id
            transaction_path = $transactionFull
            config_path = $configFull
            whole_config_sha256 = $rollback.restored_sha256
            failure_candidate_path = $rollback.failed_candidate_path
            config_write_performed = ($rollback.status -ceq 'ROLLED_BACK_VERIFIED')
            journal_write_performed = $true
            raw_credential_values_recorded = $false
        }
    }

    $displacedTerminalSnapshot = $(if (
        $record.PSObject.Properties.Name -contains 'displaced_external_sha256' -and
        [string]$record.displaced_external_sha256 -match '^[0-9a-f]{64}$' -and
        $record.PSObject.Properties.Name -contains 'displaced_external_acl_sddl' -and
        -not [string]::IsNullOrWhiteSpace([string]$record.displaced_external_acl_sddl) -and
        $record.PSObject.Properties.Name -contains 'displaced_external_attributes' -and
        $null -ne $record.displaced_external_attributes
    ) {
        [pscustomobject]@{
            exists = $true
            sha256 = [string]$record.displaced_external_sha256
            acl_sddl = [string]$record.displaced_external_acl_sddl
            attributes = [int]$record.displaced_external_attributes
        }
    } else { $null })
    $displacedTerminal = (
        [string]$record.phase -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' -and
        [string]$record.status -ceq 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE' -and
        $null -ne $displacedTerminalSnapshot -and
        (Test-SmConfigSnapshotsEqual -Left $displacedTerminalSnapshot -Right $current)
    )
    if ($displacedTerminal) {
        if ($artifactCleanupChanged) {
            Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
        }
        return [pscustomobject][ordered]@{
            schema = 'stage14-config-recovery-result/v2'
            status = 'CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE'
            transaction_id = $record.transaction_id
            transaction_path = $transactionFull
            config_path = $configFull
            whole_config_sha256 = $current.sha256
            config_write_performed = $false
            journal_write_performed = $artifactCleanupChanged
            raw_credential_values_recorded = $false
        }
    }

    $observedConflictSnapshot = $(if (
        $record.PSObject.Properties.Name -contains 'recovery_observed_exists' -and
        $null -ne $record.recovery_observed_exists
    ) {
        [pscustomobject]@{
            exists = [bool]$record.recovery_observed_exists
            sha256 = $record.recovery_observed_sha256
            acl_sddl = $record.recovery_observed_acl_sddl
            attributes = $record.recovery_observed_attributes
        }
    } else { $null })
    $alreadyConflict = (
        [string]$record.phase -ceq 'RECOVERY_CONFLICT_EXTERNAL_CHANGE' -and
        [string]$record.status -ceq 'RECOVERY_CONFLICT_EXTERNAL_CHANGE' -and
        $null -ne $observedConflictSnapshot -and
        (Test-SmConfigSnapshotsEqual -Left $observedConflictSnapshot -Right $current) -and
        -not $artifactCleanupChanged
    )
    if (-not $alreadyConflict) {
        $record.phase = 'RECOVERY_CONFLICT_EXTERNAL_CHANGE'
        $record.status = 'RECOVERY_CONFLICT_EXTERNAL_CHANGE'
        $record.error_code = $recoveryConflictCode
        foreach ($observedField in @(
            @{ name = 'recovery_observed_exists'; value = [bool]$current.exists },
            @{ name = 'recovery_observed_sha256'; value = $current.sha256 },
            @{ name = 'recovery_observed_acl_sddl'; value = $current.acl_sddl },
            @{ name = 'recovery_observed_attributes'; value = $current.attributes }
        )) {
            if ($record.PSObject.Properties.Name -contains [string]$observedField.name) {
                $record.([string]$observedField.name) = $observedField.value
            } else {
                $record | Add-Member `
                    -NotePropertyName ([string]$observedField.name) `
                    -NotePropertyValue $observedField.value
            }
        }
        Write-SmAtomicJsonSafe -Path $transactionFull -Value $record
    }
    return [pscustomobject][ordered]@{
        schema = 'stage14-config-recovery-result/v2'
        status = 'RECOVERY_CONFLICT_EXTERNAL_CHANGE'
        transaction_id = $record.transaction_id
        transaction_path = $transactionFull
        config_path = $configFull
        whole_config_sha256 = $current.sha256
        config_write_performed = $false
        journal_write_performed = (-not $alreadyConflict)
        raw_credential_values_recorded = $false
    }
}

function Test-SmPayloadManifest {
    param(
        [Parameter(Mandatory=$true)][string]$PayloadRoot,
        [string]$ManifestPath
    )
    $root = [System.IO.Path]::GetFullPath($PayloadRoot)
    if (-not $ManifestPath) { $ManifestPath = Join-Path $root 'payload-manifest.json' }
    $manifest = Read-SmJson -Path $ManifestPath
    if ($manifest.schema -ne 'stage14-payload-manifest/v1') { throw 'Unsupported payload manifest schema.' }
    if ([string]::IsNullOrWhiteSpace($manifest.version_id) -or $manifest.version_id -notmatch '^[A-Za-z0-9._+-]+$') {
        throw 'Invalid payload version_id.'
    }
    if (-not $manifest.files -or $manifest.files.Count -eq 0) { throw 'Payload manifest has no files.' }
    $checks = New-Object System.Collections.Generic.List[object]
    foreach ($record in $manifest.files) {
        $path = Resolve-SmChildPath -Root $root -RelativePath ([string]$record.path)
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        $actualHash = $null
        $actualBytes = $null
        if ($exists) {
            $actualHash = Get-SmSha256File -Path $path
            $actualBytes = (Get-Item -LiteralPath $path).Length
        }
        $ok = $exists -and $actualHash -eq ([string]$record.sha256).ToLowerInvariant() -and $actualBytes -eq [long]$record.bytes
        $checks.Add([pscustomobject]@{ path=$record.path; exists=$exists; bytes_match=($actualBytes -eq [long]$record.bytes); sha256_match=($actualHash -eq ([string]$record.sha256).ToLowerInvariant()); ok=$ok })
    }
    $entrypoints = @('mcp','hook','manager')
    foreach ($name in $entrypoints) {
        $relative = [string]$manifest.entrypoints.$name
        if (-not $relative) { throw "Missing payload entrypoint: $name" }
        $entry = Resolve-SmChildPath -Root $root -RelativePath $relative
        if (-not (Test-Path -LiteralPath $entry -PathType Leaf)) { throw "Missing payload entrypoint file: $name" }
    }
    $okAll = -not ($checks | Where-Object { -not $_.ok })
    return [pscustomobject]@{
        schema = 'stage14-payload-verification/v1'
        status = $(if ($okAll) { 'PASS' } else { 'RED_PAYLOAD_INTEGRITY_FAILURE' })
        version_id = $manifest.version_id
        manifest_sha256 = Get-SmSha256File -Path $ManifestPath
        checks = $checks.ToArray()
        manifest = $manifest
    }
}

function Get-SmCurrentPayload {
    param([Parameter(Mandatory=$true)][string]$InstallRoot)
    $root = [System.IO.Path]::GetFullPath($InstallRoot)
    $pointerPath = Join-Path $root 'state\current.json'
    $pointer = Read-SmJson -Path $pointerPath
    if ($pointer.schema -ne 'stage14-current-pointer/v1') { throw 'Unsupported current pointer schema.' }
    if ([string]::IsNullOrWhiteSpace($pointer.version_id) -or $pointer.version_id -notmatch '^[A-Za-z0-9._+-]+$') {
        throw 'Invalid current pointer version_id.'
    }
    $payloadRoot = Resolve-SmChildPath -Root $root -RelativePath ("app/versions/{0}" -f $pointer.version_id)
    $manifestPath = Join-Path $payloadRoot 'payload-manifest.json'
    $verified = Test-SmPayloadManifest -PayloadRoot $payloadRoot -ManifestPath $manifestPath
    if ($verified.status -ne 'PASS') { throw $verified.status }
    if ($verified.manifest_sha256 -ne ([string]$pointer.manifest_sha256).ToLowerInvariant()) {
        throw 'RED_PAYLOAD_INTEGRITY_FAILURE: current manifest hash mismatch.'
    }
    return [pscustomobject]@{ pointer=$pointer; pointer_path=$pointerPath; payload_root=$payloadRoot; verification=$verified }
}

function New-SmVerificationReceipt {
    param([Parameter(Mandatory=$true)]$Verification,[Parameter(Mandatory=$true)][string]$PayloadRoot)
    $files=@()
    foreach($record in $Verification.manifest.files){$p=Resolve-SmChildPath -Root $PayloadRoot -RelativePath ([string]$record.path);$i=Get-Item -LiteralPath $p;$files += [ordered]@{path=$record.path;bytes=$i.Length;last_write_utc_ticks=$i.LastWriteTimeUtc.Ticks;sha256=$record.sha256}}
    return [ordered]@{schema='stage14-verification-receipt/v1';version_id=$Verification.version_id;manifest_sha256=$Verification.manifest_sha256;files=$files;verified_at=[DateTime]::UtcNow.ToString('o')}
}

function Get-SmCurrentPayloadFast {
    param([Parameter(Mandatory=$true)][string]$InstallRoot)
    $root=[IO.Path]::GetFullPath($InstallRoot);$pointer=Read-SmJson -Path (Join-Path $root 'state\current.json')
    if($pointer.schema-ne'stage14-current-pointer/v1'-or-not $pointer.receipt_sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: current pointer lacks receipt.'}
    $payload=Resolve-SmChildPath -Root $root -RelativePath ("app/versions/{0}"-f $pointer.version_id)
    $manifestPath=Join-Path $payload 'payload-manifest.json';$receiptPath=Join-Path $payload 'verification-receipt.json'
    if((Get-SmSha256File $manifestPath)-ne $pointer.manifest_sha256-or(Get-SmSha256File $receiptPath)-ne $pointer.receipt_sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: pointer binding mismatch.'}
    $receipt=Read-SmJson $receiptPath;if($receipt.version_id-ne$pointer.version_id-or$receipt.manifest_sha256-ne$pointer.manifest_sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: receipt binding mismatch.'}
    $verified=Test-SmPayloadManifest -PayloadRoot $payload -ManifestPath $manifestPath
    if($verified.status-ne'PASS'-or$verified.manifest_sha256-ne$pointer.manifest_sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: full payload SHA256 verification failed.'}
    if(@($receipt.files).Count-ne@($verified.manifest.files).Count){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: receipt file set mismatch.'}
    for($i=0;$i-lt@($verified.manifest.files).Count;$i++){$r=$receipt.files[$i];$m=$verified.manifest.files[$i];if($r.path-ne$m.path-or[long]$r.bytes-ne[long]$m.bytes-or$r.sha256-ne$m.sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: receipt manifest binding mismatch.'}}
    return [pscustomobject]@{pointer=$pointer;payload_root=$payload;verification=$verified}
}

Export-ModuleMember -Function Get-SmSha256File,Get-SmSha256Text,Write-SmUtf8NoBom,Write-SmJson,Read-SmJson,Get-SmDefaultInstallRoot,Test-SmSafeRelativePath,Resolve-SmChildPath,Get-SmManagedConfigState,Compare-SmManagedConfig,Get-SmDesiredManagedConfigState,Get-SmManagedConfigRepairPreview,Test-SmManagedConfigRepair,Invoke-SmManagedConfigRepair,Recover-SmManagedConfigTransaction,Test-SmPayloadManifest,Get-SmCurrentPayload,New-SmVerificationReceipt,Get-SmCurrentPayloadFast,Get-SmDirectoryIdentitySnapshot,Enter-SmStableDirectoryLease,Enter-SmStableDirectoryAnchorLease,Exit-SmStableDirectoryLease
