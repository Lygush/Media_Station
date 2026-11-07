@echo off
setlocal enabledelayedexpansion

set "SKIP_JDK_VERSION_CHECK=true"

:: PROJECT_ROOT = folder containing tools\
set "PROJECT_ROOT=%~dp0.."
set "JAVA_HOME=%PROJECT_ROOT%\tools\jdk"
set "PATH=%JAVA_HOME%\bin;%PATH%"

:: DEPLOY_ROOT from argument or project root
if "%~1"=="" (
    set "DEPLOY_ROOT=%PROJECT_ROOT%"
    echo [INFO] No build path given - installing into project root.
) else (
    set "DEPLOY_ROOT=%~1"
    echo [INFO] Deploy target: %~1
)

:: All paths use delayed expansion to handle spaces in DEPLOY_ROOT
set "SDK_ROOT=!DEPLOY_ROOT!\tools\android-sdk"
:: IMPORTANT: MediaStation looks for the AVD in <app>\tools\android-tv\avd
:: (AndroidTVModule::avdHomePath). Older versions of this script created it in
:: <app>\android-tv\avd, so the app started the emulator with an empty AVD.
set "AVD_HOME=!DEPLOY_ROOT!\tools\android-tv\avd"
set "CONFIG_DIR=!DEPLOY_ROOT!\tools\android-tv\config"
set "BLOCKLIST_DIR=!DEPLOY_ROOT!\tools\android-tv\blocklists"
set "CMDLINE_ZIP=%PROJECT_ROOT%\tools\cmdline-tools-latest-windows.zip"
set "SDKMANAGER=!SDK_ROOT!\cmdline-tools\latest\bin\sdkmanager.bat"
set "AVDMANAGER=!SDK_ROOT!\cmdline-tools\latest\bin\avdmanager.bat"

echo.
echo ============================================================
echo   MediaStation - Android SDK Setup
echo ============================================================
echo.
echo   SDK root : !SDK_ROOT!
echo   AVD home : !AVD_HOME!
echo.

:: Check JDK
if not exist "%JAVA_HOME%\bin\java.exe" (
    echo [ERROR] JDK not found: %JAVA_HOME%\bin\java.exe
    echo         Place portable JDK into tools\jdk\
    echo         so that tools\jdk\bin\java.exe exists.
    echo         Download: https://adoptium.net/
    pause
    exit /b 1
)
echo [OK] JDK found: %JAVA_HOME%

:: Check zip
if not exist "%CMDLINE_ZIP%" (
    echo [ERROR] File not found: %CMDLINE_ZIP%
    echo         Download commandlinetools-win-XXXXXX_latest.zip from:
    echo         https://developer.android.com/studio#command-line-tools-only
    echo         Save as: tools\cmdline-tools-latest-windows.zip
    pause
    exit /b 1
)
echo [OK] cmdline-tools zip found

echo.
echo [1/5] Unpacking cmdline-tools...

if exist "!SDKMANAGER!" (
    echo [SKIP] cmdline-tools already extracted
) else (
    mkdir "!SDK_ROOT!\cmdline-tools\latest" 2>nul
    mkdir "!SDK_ROOT!\temp_extract" 2>nul
    (
        echo $ErrorActionPreference = 'SilentlyContinue'
        echo Expand-Archive -LiteralPath '!CMDLINE_ZIP!' -DestinationPath '!SDK_ROOT!\temp_extract' -Force
    ) > "!SDK_ROOT!\unzip.ps1"
    powershell -NoProfile -ExecutionPolicy Bypass -File "!SDK_ROOT!\unzip.ps1" 2>nul
    del "!SDK_ROOT!\unzip.ps1" 2>nul
    if not exist "!SDK_ROOT!\temp_extract\cmdline-tools\bin\sdkmanager.bat" (
        echo [ERROR] Extraction failed or ZIP has unexpected structure.
        echo         Expected: temp_extract\cmdline-tools\bin\sdkmanager.bat
        echo         ZIP may be corrupted. Re-download from:
        echo         https://developer.android.com/studio#command-line-tools-only
        pause
        exit /b 1
    )
    xcopy /e /i /q "!SDK_ROOT!\temp_extract\cmdline-tools\*" "!SDK_ROOT!\cmdline-tools\latest\" >nul
    rmdir /s /q "!SDK_ROOT!\temp_extract" 2>nul
    echo [OK] cmdline-tools extracted
)

set "ANDROID_SDK_ROOT=!SDK_ROOT!"
set "ANDROID_HOME=!SDK_ROOT!"
set "ANDROID_AVD_HOME=!AVD_HOME!"
set "ANDROID_EMULATOR_HOME=!AVD_HOME!"

echo.
echo [2/5] Accepting SDK licenses...
:: There are several licenses - a single "y" is not enough
(for /l %%i in (1,1,30) do @echo y) | call "!SDKMANAGER!" --sdk_root="!SDK_ROOT!" --licenses >nul 2>&1
echo [OK] Licenses accepted

echo.
echo [3/5] Downloading SDK components (5-15 min, about 1.5 GB)...
echo       platform-tools, emulator, system-images;android-34;android-tv;x86
echo.

call "!SDKMANAGER!" --sdk_root="!SDK_ROOT!" "platform-tools" "emulator" "system-images;android-34;android-tv;x86"

if !errorlevel! neq 0 (
    echo [ERROR] sdkmanager failed. Check your internet connection.
    pause
    exit /b 1
)
echo.
echo [OK] SDK components downloaded

echo.
echo [4/5] Creating AVD MediaStation_TV...
mkdir "!AVD_HOME!" 2>nul

:: Migrate an AVD created by an older version of this script (android-tv\avd)
if exist "!DEPLOY_ROOT!\android-tv\avd\MediaStation_TV.avd\config.ini" (
    if not exist "!AVD_HOME!\MediaStation_TV.avd\config.ini" (
        echo [INFO] Moving AVD from old location android-tv\avd ...
        xcopy /e /i /q /y "!DEPLOY_ROOT!\android-tv\avd\*" "!AVD_HOME!\" >nul
    )
)

if exist "!AVD_HOME!\MediaStation_TV.avd\config.ini" (
    echo [SKIP] AVD MediaStation_TV already exists
    goto avd_done
)

echo no | call "!AVDMANAGER!" create avd --name "MediaStation_TV" --package "system-images;android-34;android-tv;x86" --path "!AVD_HOME!\MediaStation_TV.avd" --device "tv_1080p"

if !errorlevel! neq 0 (
    echo [ERROR] avdmanager failed to create the AVD.
    pause
    exit /b 1
)

:: hw.ramSize / hw.gpu.* / showDeviceFrame are written by MediaStation on every
:: launch (patchAvdConfig); appending them here only produced duplicate keys.
echo [OK] AVD created

:avd_done


echo.
echo [5/5] Creating android-tv config files...
mkdir "!CONFIG_DIR!" 2>nul
mkdir "!BLOCKLIST_DIR!" 2>nul

:: Ad blocking: AdGuard Home (replaces dnsproxy, which the emulator never used).
:: MediaStation writes AdGuardHome.yaml and installs the Windows service from
:: Settings - Android TV - "Install service" (one UAC prompt).
set "AGH_DIR=!DEPLOY_ROOT!\tools\adguardhome"
if exist "!AGH_DIR!\AdGuardHome.exe" (
    echo [SKIP] AdGuard Home already present
) else (
    echo Downloading AdGuard Home...
    curl.exe -L --fail -o "%TEMP%\AdGuardHome_windows_amd64.zip" https://github.com/AdguardTeam/AdGuardHome/releases/latest/download/AdGuardHome_windows_amd64.zip
    if !errorlevel! neq 0 (
        echo [WARN] AdGuard Home download failed - ad blocking will be unavailable.
        echo        Put AdGuardHome.exe into tools\adguardhome\ manually.
    ) else (
        mkdir "!DEPLOY_ROOT!\tools" 2>nul
        tar -xf "%TEMP%\AdGuardHome_windows_amd64.zip" -C "!DEPLOY_ROOT!\tools"
        del "%TEMP%\AdGuardHome_windows_amd64.zip" 2>nul
        if exist "!AGH_DIR!\AdGuardHome.exe" (
            echo [OK] tools\adguardhome\AdGuardHome.exe
        ) else (
            echo [WARN] AdGuard Home archive unpacked, but AdGuardHome.exe not found
        )
    )
)

if exist "!BLOCKLIST_DIR!\default.txt" (
    echo [SKIP] default.txt already exists
) else (
    (
        echo # MediaStation Android TV - ad blocklist
        echo # One domain per line. Lines starting with # are comments.
        echo doubleclick.net
        echo googlesyndication.com
        echo googleadservices.com
        echo adnxs.com
        echo ads.yahoo.com
        echo scorecardresearch.com
        echo quantserve.com
        echo moatads.com
        echo pagead2.googlesyndication.com
    ) > "!BLOCKLIST_DIR!\default.txt"
    echo [OK] tools\android-tv\blocklists\default.txt created
)

echo.
echo ============================================================
echo   Setup complete!
echo.
echo   ADB      : !SDK_ROOT!\platform-tools\adb.exe
echo   Emulator : !SDK_ROOT!\emulator\emulator.exe
echo   AVD      : !AVD_HOME!\MediaStation_TV.avd\
echo.
echo   Verify with:
echo   !SDK_ROOT!\emulator\emulator.exe -list-avds
echo ============================================================
echo.
pause
