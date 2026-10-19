@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

pushd "%~dp0.."
set "nativeBuildDirectory=build\native"
set "nativeDevelopmentOptions="
set bluetoothCompilerOptions=/DONIMUSHA_BLUETOOTH_ENABLED /I"third_party\opus-1.5.2\include"
set bluetoothLinkOptions="build\opus-win-x64\Release\opus.lib"
if /I "%~1"=="--usb-only" (
  set "bluetoothCompilerOptions="
  set "bluetoothLinkOptions="
)
if /I "%~1"=="--bluetooth-development" (
  set "nativeBuildDirectory=build\native-bluetooth-dev"
  set "nativeDevelopmentOptions=/DONIMUSHA_LOCAL_DEVELOPMENT"
)
if not exist "%nativeBuildDirectory%" mkdir "%nativeBuildDirectory%"

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  %bluetoothCompilerOptions% %nativeDevelopmentOptions% "src\native\dualsense_hid_helper.cpp" ^
  /Fo:"%nativeBuildDirectory%\dualsense_hid_helper.obj" ^
  /Fe:"%nativeBuildDirectory%\codex_dualsense_hid_helper.exe" ^
  %bluetoothLinkOptions% ole32.lib setupapi.lib hid.lib propsys.lib bcrypt.lib crypt32.lib advapi32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE /LD ^
  %nativeDevelopmentOptions% ^
  /I "third_party\reframework\include" ^
  "src\native\dualsense_bridge.cpp" ^
  /Fo:"%nativeBuildDirectory%\dualsense_bridge.obj" ^
  /Fe:"%nativeBuildDirectory%\codex_dualsense_bridge.dll" ^
  /link /IMPLIB:"%nativeBuildDirectory%\codex_dualsense_bridge.lib" xinput.lib
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\native_product_runtime_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\native_product_runtime_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\native_product_runtime_smoke.exe" ^
  bcrypt.lib crypt32.lib advapi32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\dualsense_hid_report_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\dualsense_hid_report_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\dualsense_hid_report_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /I "src\native" ^
  "tests\dualsense_touch_input_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\dualsense_touch_input_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\dualsense_touch_input_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\melee_repel_detection_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\melee_repel_detection_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\melee_repel_detection_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\repel_impact_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\repel_impact_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\repel_impact_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\oni_strength_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\oni_strength_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\oni_strength_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\oni_charge_release_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\oni_charge_release_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\oni_charge_release_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\oni_transformation_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\oni_transformation_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\oni_transformation_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\combat_speaker_timing_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\combat_speaker_timing_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\combat_speaker_timing_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\black_soul_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\black_soul_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\black_soul_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\wall_run_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\wall_run_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\wall_run_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /I "src\native" ^
  "tests\live_motor_state_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\live_motor_state_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\live_motor_state_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\ladder_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\ladder_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\ladder_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\normal_attack_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\normal_attack_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\normal_attack_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\execution_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\execution_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\execution_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /I "src\native" ^
  "tests\treasure_chest_haptics_smoke.cpp" ^
  /Fo:"%nativeBuildDirectory%\treasure_chest_haptics_smoke.obj" ^
  /Fe:"%nativeBuildDirectory%\treasure_chest_haptics_smoke.exe"
if errorlevel 1 exit /b %errorlevel%

if not "%bluetoothCompilerOptions%"=="" (
  cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /I "src\native" "tests\speaker_preview_smoke.cpp" /Fo:"%nativeBuildDirectory%\speaker_preview_smoke.obj" /Fe:"%nativeBuildDirectory%\speaker_preview_smoke.exe" bcrypt.lib crypt32.lib advapi32.lib user32.lib
  if errorlevel 1 exit /b 1
  cl.exe /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /I "src\native" /I "third_party\opus-1.5.2\include" "tests\dualsense_bluetooth_smoke.cpp" /Fo:"%nativeBuildDirectory%\dualsense_bluetooth_smoke.obj" /Fe:"%nativeBuildDirectory%\dualsense_bluetooth_smoke.exe" %bluetoothLinkOptions%
  if errorlevel 1 exit /b 1
)

popd
endlocal
