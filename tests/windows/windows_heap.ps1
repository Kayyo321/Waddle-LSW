# Build and run the production guest C paths with the MSVC debug CRT.
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs_path = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs_path) { throw 'MSVC installation unavailable' }
$dev_module = Join-Path $vs_path 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $dev_module
Enter-VsDevShell -VsInstallPath $vs_path -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
zig build-lib src/guest/guest_codec.zig -static -target x86_64-windows-msvc -O ReleaseSafe -fno-compiler-rt '-femit-bin=build\guest_heap_codec.lib'
if ($LASTEXITCODE -ne 0) { throw 'MSVC ABI codec build failed' }
$compiler_flags = @('/nologo', '/std:c11', '/MDd', '/Od', '/Zi', '/D_DEBUG', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000006', '/D_CRT_SECURE_NO_WARNINGS', '/Iinclude', '/Isrc', '/Isrc/common', '/Isrc/guest', '/Itests/windows', '/FIwindows_heap.h')
$guest_sources = @('guest_session', 'guest_wire', 'guest_input', 'guest_output', 'guest_process', 'guest_environment')
foreach ($source in $guest_sources) {
    cl @compiler_flags /c "src/guest/$source.c" "/Fobuild/$source.obj"
    if ($LASTEXITCODE -ne 0) { throw "Heap build failed: $source" }
}
cl @compiler_flags /DGuestHeapListener /c src/guest/guest_listener.c /Fobuild/guest_listener.obj
if ($LASTEXITCODE -ne 0) { throw 'Heap listener build failed' }
$guest_objects = @('build/guest_listener.obj') + @($guest_sources | ForEach-Object { "build/$_.obj" })
link /nologo /debug @guest_objects build/guest_heap_codec.lib ws2_32.lib ntdll.lib /out:build/waddle-guest-exec.exe
if ($LASTEXITCODE -ne 0) { throw 'Heap guest link failed' }
cl /nologo /std:c11 /MDd /Od /Zi /D_DEBUG /D_CRT_SECURE_NO_WARNINGS tests/windows/windows_heap_probe.c /Febuild/windows_heap_probe.exe /link ws2_32.lib
if ($LASTEXITCODE -ne 0) { throw 'Heap detector probe build failed' }
foreach ($mode in @('leak', 'overrun', 'freed-write')) {
    & ./build/windows_heap_probe.exe $mode
    if ($LASTEXITCODE -ne 86) { throw "Heap detector missed negative control: $mode" }
}
Remove-Item build/windows_heap.log, build/windows_heap_errors.log -ErrorAction SilentlyContinue
& ./build/windows_guest_test.exe
if ($LASTEXITCODE -ne 0) {
    Get-Content build/windows_heap_errors.log -ErrorAction SilentlyContinue
    Get-Content build/windows_heap.log -ErrorAction SilentlyContinue
    throw 'Instrumented native regression failed'
}
$checks = @(Get-Content build/windows_heap.log)
if ($checks.Count -ne 47) { throw "Expected 47 heap checkpoints, got $($checks.Count)" }
foreach ($line in $checks) {
    if ($line -notmatch '^session \d+: 0 allocations, 0 bytes, intact heap$') { throw "Invalid heap checkpoint: $line" }
}
$checks | Write-Output
