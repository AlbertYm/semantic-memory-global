param(
    [string]$BuildDir = "build/c"
)

$ErrorActionPreference = "Stop"

function Find-FirstExistingPath {
    param([string[]]$Paths)
    foreach ($path in $Paths) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }
    return $null
}

$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
Set-Location -LiteralPath $repo

$gcc = Find-FirstExistingPath @(
    "C:/winlibs/mingw64/bin/gcc.exe",
    "C:/msys64/ucrt64/bin/gcc.exe",
    "C:/msys64/mingw64/bin/gcc.exe"
)
if (-not $gcc) {
    throw "gcc.exe not found. Install Winlibs or MSYS2 MinGW and rerun this script."
}

$gccBin = Split-Path -Parent $gcc
$msysBin = Find-FirstExistingPath @("C:/msys64/usr/bin")
if ($msysBin) {
    $env:PATH = "$gccBin;$msysBin;$env:PATH"
} else {
    $env:PATH = "$gccBin;$env:PATH"
}

$build = Join-Path $repo $BuildDir
New-Item -ItemType Directory -Force -Path $build | Out-Null

$commonFlags = @(
    "-std=c11",
    "-D_DEFAULT_SOURCE",
    "-D_GNU_SOURCE",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-Wno-format-truncation",
    "-Wno-format-overflow",
    "-Wno-unused-function",
    "-Wno-error",
    "-Isrc",
    "-Isrc/store",
    "-Isrc/foundation",
    "-Ivendored",
    "-Ivendored/sqlite3",
    "-Ivendored/tre",
    "-Ivendored/mimalloc/include",
    "-Ivendored/mimalloc/src",
    "-Iinternal/cbm",
    "-Iinternal/cbm/vendored/ts_runtime/include",
    "-g",
    "-O0",
    "-DSQLITE_DQS=0",
    "-DSQLITE_THREADSAFE=1",
    "-DSQLITE_ENABLE_FTS5"
)

$quietFlags = @("-std=c11", "-g", "-O0", "-w")

function Compile-C {
    param(
        [string]$Source,
        [string]$Object,
        [string[]]$Flags
    )
    & $gcc @Flags "-c" "-o" $Object $Source
    if ($LASTEXITCODE -ne 0) {
        throw "compile failed: $Source"
    }
}

$objects = @(
    @{ Source = "src/store/store.c"; Object = "store_clean.o"; Flags = $commonFlags },
    @{ Source = "src/memory/memory_store.c"; Object = "memory_store.o"; Flags = $commonFlags },
    @{ Source = "vendored/sqlite3/sqlite3.c"; Object = "sqlite3_clean.o"; Flags = @("-std=c11", "-g", "-O0", "-w", "-DSQLITE_DQS=0", "-DSQLITE_THREADSAFE=1", "-DSQLITE_ENABLE_FTS5") },
    @{ Source = "vendored/tre/tre_all.c"; Object = "tre.o"; Flags = @("-std=c11", "-g", "-O0", "-w", "-Ivendored/tre") },
    @{ Source = "src/foundation/compat_regex.c"; Object = "compat_regex.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/arena.c"; Object = "arena.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/str_util.c"; Object = "str_util.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/log.c"; Object = "log.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/platform.c"; Object = "platform.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/compat.c"; Object = "compat.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/mem.c"; Object = "mem.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/str_intern.c"; Object = "str_intern.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/slab_alloc.c"; Object = "slab_alloc.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/hash_table.c"; Object = "hash_table.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/system_info.c"; Object = "system_info.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/compat_thread.c"; Object = "compat_thread.o"; Flags = $commonFlags },
    @{ Source = "src/foundation/compat_proc.c"; Object = "compat_proc.o"; Flags = $commonFlags },
    @{ Source = "src/store/embed.c"; Object = "embed.o"; Flags = $commonFlags },
    @{ Source = "src/pipeline/worker_pool.c"; Object = "worker_pool.o"; Flags = $commonFlags },
    @{ Source = "src/simhash/minhash.c"; Object = "minhash.o"; Flags = ($commonFlags + @("-Iinternal/cbm/vendored/ts_runtime/include")) },
    @{ Source = "src/semantic/semantic.c"; Object = "semantic.o"; Flags = ($commonFlags + @("-Iinternal/cbm/vendored/ts_runtime/include")) },
    @{ Source = "src/semantic/rotsq.c"; Object = "rotsq.o"; Flags = ($commonFlags + @("-Iinternal/cbm/vendored/ts_runtime/include")) },
    @{ Source = "vendored/nomic/code_vectors_blob.S"; Object = "code_vectors_blob.o"; Flags = @() },
    @{ Source = "vendored/yyjson/yyjson.c"; Object = "yyjson.o"; Flags = $commonFlags },
    @{ Source = "vendored/mimalloc/src/static.c"; Object = "mimalloc_clean.o"; Flags = @("-std=c11", "-g", "-O0", "-w", "-Ivendored/mimalloc/include", "-Ivendored/mimalloc/src", "-DMI_OVERRIDE=0") },
    @{ Source = "internal/cbm/ts_runtime.c"; Object = "ts_runtime_clean.o"; Flags = @("-std=c11", "-D_DEFAULT_SOURCE", "-g", "-O0", "-w", "-Iinternal/cbm", "-Iinternal/cbm/vendored/ts_runtime/include", "-Iinternal/cbm/vendored/ts_runtime/src") }
)

foreach ($item in $objects) {
    Compile-C -Source $item.Source -Object (Join-Path $build $item.Object) -Flags $item.Flags
}

$linkObjects = @(
    "store_clean.o",
    "memory_store.o",
    "sqlite3_clean.o",
    "tre.o",
    "compat_regex.o",
    "arena.o",
    "str_util.o",
    "log.o",
    "platform.o",
    "compat.o",
    "mem.o",
    "str_intern.o",
    "slab_alloc.o",
    "hash_table.o",
    "system_info.o",
    "compat_thread.o",
    "compat_proc.o",
    "embed.o",
    "worker_pool.o",
    "minhash.o",
    "semantic.o",
    "rotsq.o",
    "code_vectors_blob.o",
    "yyjson.o",
    "mimalloc_clean.o",
    "ts_runtime_clean.o"
) | ForEach-Object { Join-Path $build $_ }

$exe = Join-Path $build "test-memory-framework.exe"
& $gcc @commonFlags "-o" $exe "tests/test_memory.c" @linkObjects "-lm" "-lpthread" "-lws2_32" "-lpsapi"
if ($LASTEXITCODE -ne 0) {
    throw "link failed: $exe"
}

& $exe
if ($LASTEXITCODE -ne 0) {
    throw "test-memory-framework.exe failed with exit code $LASTEXITCODE"
}
