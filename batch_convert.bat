@echo off
setlocal enabledelayedexpansion

echo ========================================
echo ModelConverter Batch Format Conversion
echo ========================================

REM ---- 路径配置（按实际构建目录修改）----
set "BUILD_DIR=build_debug\Debug"
set "EXE_PATH=%~dp0!BUILD_DIR!"
set "FBX_SDK_DLL=%~dp0thirdparty\fbxsdk\2020.3.9\lib\x64\debug\libfbxsdk.dll"

REM 确保可执行文件及依赖 DLL 在 PATH 中
set "PATH=!EXE_PATH!;!PATH!"

REM ---- 输入/输出目录 ----
set "INPUT_DIR=D:/Data/model/fbx_n"
set "OUTPUT_DIR=D:/Data/model/out/glb_n"

REM 创建输出目录
if not exist "!OUTPUT_DIR!" mkdir "!OUTPUT_DIR!"

echo EXE_PATH =!EXE_PATH!
echo INPUT_DIR =!INPUT_DIR!
echo OUTPUT_DIR=!OUTPUT_DIR!
echo.

REM 检查可执行文件是否存在
if not exist "!EXE_PATH!\mc_cli.exe" (
    echo [ERROR] mc_cli.exe not found in !EXE_PATH!
    echo Please build the project first or adjust BUILD_DIR.
    pause
    exit /b 1
)

REM 检查 FBX SDK DLL 是否在 exe 同目录（CMake post-build 会复制）
if not exist "!EXE_PATH!\libfbxsdk.dll" (
    if exist "!FBX_SDK_DLL!" (
        echo Copying libfbxsdk.dll to exe directory...
        copy "!FBX_SDK_DLL!" "!EXE_PATH!\" >nul
    ) else (
        echo [WARN] libfbxsdk.dll not found. FBX conversion may fail.
    )
)

REM ---- 统计 ----
set "SUCCESS_COUNT=0"
set "FAIL_COUNT=0"
set "SKIP_COUNT=0"

echo ========================================
echo Starting batch conversion...
echo ========================================
echo.

REM 遍历输入目录中所有支持格式的文件
for %%F in ("!INPUT_DIR!\*.fbx" "!INPUT_DIR!\*.gltf" "!INPUT_DIR!\*.glb" "!INPUT_DIR!\*.obj" "!INPUT_DIR!\*.dae" "!INPUT_DIR!\*.stl" "!INPUT_DIR!\*.3ds" "!INPUT_DIR!\*.ply") do (
    set "INPUT_FILE=%%~F"
    set "FILE_NAME=%%~nF"
    set "FILE_EXT=%%~xF"
    set "OUTPUT_FILE=!OUTPUT_DIR!\!FILE_NAME!.glb"

    echo [%TIME%] Processing: !FILE_NAME!!FILE_EXT! -^> !FILE_NAME!.glb

    REM 转换后的 .log 由 mc_cli 根据输出文件名自动生成
    "!EXE_PATH!\mc_cli.exe" "!INPUT_FILE!" "!OUTPUT_FILE!"
    set "EXIT_CODE=!errorlevel!"

    if !EXIT_CODE! neq 0 (
        echo [ERROR] Failed: !FILE_NAME!!FILE_EXT! ^(exit !EXIT_CODE!^)
        echo [%DATE% %TIME%] EXIT=!EXIT_CODE! INPUT=!INPUT_FILE! OUTPUT=!OUTPUT_FILE! >> "!OUTPUT_DIR!\error.log"
        set /a FAIL_COUNT+=1
    ) else (
        echo [OK]    Converted: !FILE_NAME!.glb
        set /a SUCCESS_COUNT+=1
    )
    echo.
)

echo ========================================
echo Batch conversion finished.
echo   Success : !SUCCESS_COUNT!
echo   Failed  : !FAIL_COUNT!
echo ========================================

if !FAIL_COUNT! gtr 0 (
    echo Error details saved to: !OUTPUT_DIR!\error.log
)

pause
endlocal
