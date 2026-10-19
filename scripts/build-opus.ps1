$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S (Join-Path $repository 'third_party/opus-1.5.2') -B (Join-Path $repository 'build/opus-win-x64') -G 'Visual Studio 17 2022' -A x64 '-DOPUS_PACKAGE_VERSION=1.5.2' '-DOPUS_STATIC_RUNTIME=ON' '-DOPUS_BUILD_TESTING=OFF' '-DOPUS_BUILD_PROGRAMS=OFF' '-DOPUS_DISABLE_INTRINSICS=ON'
if ($LASTEXITCODE -ne 0) { throw 'Opus configuration failed' }
& $cmake --build (Join-Path $repository 'build/opus-win-x64') --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Opus compilation failed' }
