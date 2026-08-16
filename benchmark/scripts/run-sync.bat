@echo off
setlocal EnableDelayedExpansion

REM ============================================================================
REM Xylem sync-primitive benchmark (Windows, native cmd.exe)
REM ----------------------------------------------------------------------------
REM Run from any terminal (cmd.exe / PowerShell). The build step auto-detects
REM Visual Studio via vswhere and initializes vcvars64.bat, so cl.exe is set up
REM automatically (cmake / ninja must be on PATH). See docs\build.md.
REM
REM   mutex|cond|sem|channel - build + run the full comparison matrix for that
REM                           primitive (xylem vs go vs rust, all modes)
REM   all                   - same, for every primitive   [default]
REM
REM Fixed matrix (edit the defaults below to change the suite):
REM   prims: mutex,cond,sem,channel   langs: xylem,go,rust   repeat: 1
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_NAME=%~nx0"
for %%I in ("%SCRIPT_DIR%..") do set "BENCH_DIR=%%~fI"
set "SYNC_DIR=%BENCH_DIR%\sync"
for %%I in ("%BENCH_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "OUT_DIR=%BENCH_DIR%\out"
set "BIN_DIR=%OUT_DIR%"
set "BUILD_DIR=%OUT_DIR%\build"
set "RESULTS_ROOT=%OUT_DIR%\results"

REM Fixed matrix (no CLI options -- edit these to change the suite).
set "PRIMS=mutex,cond,sem,channel"
set "LANGS=xylem,go,rust"
set "WORKERS=0"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=4"
set "NCPU=%NUMBER_OF_PROCESSORS%"

set "CMD=%~1"
if not "%CMD%"=="" shift

if /I "%CMD%"=="mutex"   goto :do_prim
if /I "%CMD%"=="cond"    goto :do_prim
if /I "%CMD%"=="sem"     goto :do_prim
if /I "%CMD%"=="channel" goto :do_prim
if /I "%CMD%"=="-h"       goto :usage
if /I "%CMD%"=="--help"   goto :usage
if /I "%CMD%"=="help"     goto :usage
if not "%CMD%"=="" (
    call :err "unknown target: %CMD% (must be mutex^|cond^|sem^|channel^|help)"
    goto :usage
)

:do_prim
if not "%~1"=="" (
    call :err "unexpected extra arguments: %*"
    exit /b 1
)
if not "%CMD%"=="" set "PRIMS=%CMD%"
call :ensure_deps || exit /b 1
call :cmd_build || exit /b 1
call :cmd_bench
goto :eof

REM ---------------------------------------------------------------- logging
:info
echo [sync] %~1
goto :eof
:ok
echo [ok] %~1
goto :eof
:warn
echo [warn] %~1
goto :eof
:err
echo [err] %~1 1>&2
goto :eof

REM ------------------------------------------------------------------ build
:ensure_msvc
where cl >nul 2>&1 && goto :eof
set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_VSWHERE%" (
    call :err "MSVC not found and vswhere is missing. Install Visual Studio with the C++ toolset, or run from a Developer Command Prompt."
    exit /b 1
)
set "_VSPATH="
for /f "usebackq tokens=*" %%i in (`"%_VSWHERE%" -latest -products * -property installationPath`) do set "_VSPATH=%%i"
if not defined _VSPATH (
    call :err "no Visual Studio install with the C++ toolset found"
    exit /b 1
)
set "_VCVARS=%_VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%_VCVARS%" (
    call :err "vcvars64.bat not found under %_VSPATH%"
    exit /b 1
)
call :info "initializing MSVC x64 environment (vcvars64.bat)..."
call "%_VCVARS%" >nul
where cl >nul 2>&1 || (call :err "cl.exe still not found after vcvars64.bat" & exit /b 1)
goto :eof

REM ============================================================================
REM dependencies (guidance printed automatically when tools are missing)
REM ============================================================================

REM ensure_deps -- abort with setup guidance when cmake is missing; the build
REM step itself reports missing go/rust/cl (ensure_msvc) with clear errors.
:ensure_deps
where cmake >nul 2>&1 || (
    call :err "required tool missing: cmake"
    call :cmd_install
    exit /b 1
)
goto :eof

:cmd_install
call :info "Windows dependency setup"
echo   Required toolchain (run from an elevated PowerShell / cmd):
echo.
echo     winget install Kitware.CMake
echo     winget install Ninja-build.Ninja
echo     winget install GoLang.Go
echo     winget install Rustlang.Rustup
echo.
echo   Visual Studio 2022 (MSVC C/C++ toolset) must be installed. The build
echo   step auto-detects it via vswhere and initializes vcvars64.bat, so a
echo   plain terminal works; a "Developer Command Prompt" also works.
echo.
where cl >nul 2>&1
if errorlevel 1 (
    call :warn "cl.exe not on PATH now; build will auto-init MSVC via vcvars64.bat."
) else (
    call :ok "cl.exe detected"
)
goto :eof

:cmd_build
call :ensure_msvc || exit /b 1
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

REM Force MSVC: if an existing CMake cache pinned a non-cl compiler (e.g. a
REM MinGW gcc found earlier on PATH), wipe it so we reconfigure with cl.exe.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /I /C:"CMAKE_C_COMPILER:" "%BUILD_DIR%\CMakeCache.txt" | findstr /I "cl.exe" >nul || (
        call :info "stale non-MSVC CMake cache found; reconfiguring..."
        rmdir /s /q "%BUILD_DIR%"
    )
)

set "USE_NINJA=false"
where ninja >nul 2>&1 && set "USE_NINJA=true"

call :info "building xylem static library..."
if "%USE_NINJA%"=="true" (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl >nul 2>&1
    cmake --build "%BUILD_DIR%" --target xylem -j %NCPU% >nul 2>&1
) else (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -DCMAKE_C_COMPILER=cl >nul 2>&1
    cmake --build "%BUILD_DIR%" --target xylem --config Release -j %NCPU% >nul 2>&1
)
call :find_xylem_lib
if not defined XYLEM_LIB (
    call :err "could not locate built xylem static library under %BUILD_DIR%"
    exit /b 1
)
call :ok "xylem built (%XYLEM_LIB%)"

set "CL_FLAGS=/nologo /std:c11 /experimental:c11atomics /O2 /DNDEBUG /MD /W3 /I\"%PROJECT_ROOT%\src\""
set "SYS_LIBS=ws2_32.lib mswsock.lib psapi.lib"

echo %LANGS% | findstr /I "xylem" >nul && (
    call :info "building xylem sync-bench..."
    echo %PRIMS% | findstr /I "mutex" >nul && (
        if exist "%BIN_DIR%\mutex-xylem.exe" del /q "%BIN_DIR%\mutex-xylem.exe"
        cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SYNC_DIR%\mutex\xylem\main.c" "%XYLEM_LIB%" %SYS_LIBS% /Fe:"%BIN_DIR%\mutex-xylem.exe" >nul 2>&1
        if errorlevel 1 (call :err "mutex-xylem build failed" & exit /b 1) else (call :ok "mutex-xylem built")
    )
    echo %PRIMS% | findstr /I "cond" >nul && (
        if exist "%BIN_DIR%\cond-xylem.exe" del /q "%BIN_DIR%\cond-xylem.exe"
        cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SYNC_DIR%\cond\xylem\main.c" "%XYLEM_LIB%" %SYS_LIBS% /Fe:"%BIN_DIR%\cond-xylem.exe" >nul 2>&1
        if errorlevel 1 (call :err "cond-xylem build failed" & exit /b 1) else (call :ok "cond-xylem built")
    )
    echo %PRIMS% | findstr /I "sem" >nul && (
        if exist "%BIN_DIR%\sem-xylem.exe" del /q "%BIN_DIR%\sem-xylem.exe"
        cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SYNC_DIR%\sem\xylem\main.c" "%XYLEM_LIB%" %SYS_LIBS% /Fe:"%BIN_DIR%\sem-xylem.exe" >nul 2>&1
        if errorlevel 1 (call :err "sem-xylem build failed" & exit /b 1) else (call :ok "sem-xylem built")
    )
    echo %PRIMS% | findstr /I "channel" >nul && (
        if exist "%BIN_DIR%\channel-xylem.exe" del /q "%BIN_DIR%\channel-xylem.exe"
        cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SYNC_DIR%\channel\xylem\main.c" "%XYLEM_LIB%" %SYS_LIBS% /Fe:"%BIN_DIR%\channel-xylem.exe" >nul 2>&1
        if errorlevel 1 (call :err "channel-xylem build failed" & exit /b 1) else (call :ok "channel-xylem built")
    )
)

echo %LANGS% | findstr /I "rust" >nul && (
    echo %PRIMS% | findstr /I "mutex" >nul && (
        call :info "building mutex-rust..."
        pushd "%SYNC_DIR%\mutex\rust"
        cargo build --release -q --target-dir "%BIN_DIR%\cargo" >nul 2>&1 && copy /Y "%BIN_DIR%\cargo\release\mutex-rust.exe" "%BIN_DIR%\" >nul && (call :ok "mutex-rust built") || (call :warn "skip mutex-rust")
        popd
    )
    echo %PRIMS% | findstr /I "cond" >nul && (
        call :info "building cond-rust..."
        pushd "%SYNC_DIR%\cond\rust"
        cargo build --release -q --target-dir "%BIN_DIR%\cargo" >nul 2>&1 && copy /Y "%BIN_DIR%\cargo\release\cond-rust.exe" "%BIN_DIR%\" >nul && (call :ok "cond-rust built") || (call :warn "skip cond-rust")
        popd
    )
    echo %PRIMS% | findstr /I "sem" >nul && (
        call :info "building sem-rust..."
        pushd "%SYNC_DIR%\sem\rust"
        cargo build --release -q --target-dir "%BIN_DIR%\cargo" >nul 2>&1 && copy /Y "%BIN_DIR%\cargo\release\sem-rust.exe" "%BIN_DIR%\" >nul && (call :ok "sem-rust built") || (call :warn "skip sem-rust")
        popd
    )
    echo %PRIMS% | findstr /I "channel" >nul && (
        call :info "building channel-rust..."
        pushd "%SYNC_DIR%\channel\rust"
        cargo build --release -q --target-dir "%BIN_DIR%\cargo" >nul 2>&1 && copy /Y "%BIN_DIR%\cargo\release\channel-rust.exe" "%BIN_DIR%\" >nul && (call :ok "channel-rust built") || (call :warn "skip channel-rust")
        popd
    )
)

echo %LANGS% | findstr /I "go" >nul && (
    where go >nul 2>&1
    if not errorlevel 1 (
        echo %PRIMS% | findstr /I "mutex" >nul && (
            if exist "%BIN_DIR%\mutex-go.exe" del /q "%BIN_DIR%\mutex-go.exe"
            call :info "building mutex-go..."
            pushd "%SYNC_DIR%\mutex\go"
            set "CGO_ENABLED=0"
            go build -ldflags="-s -w" -o "%BIN_DIR%\mutex-go.exe" . && (call :ok "mutex-go built") || (popd & call :err "mutex-go build failed" & exit /b 1)
            popd
        )
        echo %PRIMS% | findstr /I "cond" >nul && (
            if exist "%BIN_DIR%\cond-go.exe" del /q "%BIN_DIR%\cond-go.exe"
            call :info "building cond-go..."
            pushd "%SYNC_DIR%\cond\go"
            set "CGO_ENABLED=0"
            go build -ldflags="-s -w" -o "%BIN_DIR%\cond-go.exe" . && (call :ok "cond-go built") || (popd & call :err "cond-go build failed" & exit /b 1)
            popd
        )
        echo %PRIMS% | findstr /I "channel" >nul && (
            if exist "%BIN_DIR%\channel-go.exe" del /q "%BIN_DIR%\channel-go.exe"
            call :info "building channel-go..."
            pushd "%SYNC_DIR%\channel\go"
            set "CGO_ENABLED=0"
            go build -ldflags="-s -w" -o "%BIN_DIR%\channel-go.exe" . && (call :ok "channel-go built") || (popd & call :err "channel-go build failed" & exit /b 1)
            popd
        )
    ) else (
        call :err "go not found"
        exit /b 1
    )
)

echo.
dir /a-d "%BIN_DIR%"
goto :eof

:find_xylem_lib
set "XYLEM_LIB="
if exist "%BUILD_DIR%\xylem.lib"         set "XYLEM_LIB=%BUILD_DIR%\xylem.lib" & goto :eof
if exist "%BUILD_DIR%\Release\xylem.lib" set "XYLEM_LIB=%BUILD_DIR%\Release\xylem.lib" & goto :eof
for /r "%BUILD_DIR%" %%F in (xylem.lib) do (
    if exist "%%F" set "XYLEM_LIB=%%F" & goto :eof
)
goto :eof

REM ------------------------------------------------------------------ bench
:cmd_bench
for /f "tokens=2 delims==" %%T in ('wmic os get localdatetime /value 2^>nul ^| find "="') do set "_DT=%%T"
set "TS=%_DT:~0,8%-%_DT:~8,6%"
set "RUN_DIR=%RESULTS_ROOT%\%TS%"
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"

call :info "results: %RUN_DIR%   workers=%WORKERS%"
call :info "prims: %PRIMS%   langs: %LANGS%"
echo.

for %%P in (%PRIMS:,= %) do call :bench_prim %%P

call :ok "sync benchmarks complete"
call :info "results written to %RUN_DIR%"
goto :eof

REM bench_prim <prim>
:bench_prim
set "PRIM=%~1"
if /I "%PRIM%"=="mutex" (
    call :bench_mutex
    goto :eof
)
if /I "%PRIM%"=="cond" (
    call :bench_cond
    goto :eof
)
if /I "%PRIM%"=="sem" (
    call :bench_sem
    goto :eof
)
if /I "%PRIM%"=="channel" (
    call :bench_channel
    goto :eof
)
goto :eof

REM nspo_to_x100 <decimal> -> _NSPO_X100
:nspo_to_x100
set "_NSPO_X100=0"
set "_nspo_i=0"
set "_nspo_f=0"
for /f "tokens=1,2 delims=." %%A in ("%~1") do (
    set "_nspo_i=%%A"
    set "_nspo_f=%%B"
)
set "_nspo_f=!_nspo_f!00"
set "_nspo_f=!_nspo_f:~0,2!"
set /a _NSPO_X100=_nspo_i*100+_nspo_f
goto :eof

:bench_mutex
call :info "=== mutex  (tasks=2*workers, 5s) ==="
echo   LANG    MODE        ops/s        ns/op     total_ops
echo   ----------------------------------------------------

for %%L in (%LANGS:,= %) do (
    set "lang=%%L"
    set "BIN="
    set "modes="
    if /I "!lang!"=="xylem" (set "BIN=%BIN_DIR%\mutex-xylem.exe" & set "modes=cc tt ct tc")
    if /I "!lang!"=="go"    (set "BIN=%BIN_DIR%\mutex-go.exe"    & set "modes=cc")
    if /I "!lang!"=="rust"  (set "BIN=%BIN_DIR%\mutex-rust.exe"  & set "modes=cc tt")
    if not defined BIN (
        call :warn "skip !lang! (mutex unsupported)"
    ) else if not exist "!BIN!" (
        call :warn "skip !lang! (no binary)"
    ) else (
        for %%M in (!modes!) do (
            set "mode=%%M"
            set "out=%RUN_DIR%\sync-mutex-!lang!-!mode!.json"
            "!BIN!" > "!out!" 2>nul

            set "ops="
            set "nspo="
            set "total="
            for %%F in ("!out!") do set "_sz=%%~zF"
            if defined _sz if !_sz! GTR 0 (
                for /f "tokens=1-3" %%A in ('powershell -NoProfile -Command "$m='!mode!'; $txt=gc '!out!' -Raw; $blocks=$txt -split '(?<=\})\s*\r?\n\s*(?=\{)'; foreach($b in $blocks){if($b -match ('\"mode\":\s*\"' + $m + '\"')){$o='';$n='';$t='';if($b -match '\"ops_per_sec\":\s*([0-9.]+)'){$o=$Matches[1]};if($b -match '\"ns_per_op\":\s*([0-9.]+)'){$n=$Matches[1]};if($b -match '\"total_ops\":\s*([0-9]+)'){$t=$Matches[1]};Write-Output ($o + ' ' + $n + ' ' + $t)}}"') do (
                    set "ops=%%A"
                    set "nspo=%%B"
                    set "total=%%C"
                )
                for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                if defined ops if !ops! GTR 0 (
                    call :nspo_to_x100 "!nspo!"
                    set /a nspo_i=_NSPO_X100/100
                    set /a nspo_f=_NSPO_X100%%100
                    if !nspo_f! LSS 10 (set "nspo_fmt=!nspo_i!.0!nspo_f!") else (set "nspo_fmt=!nspo_i!.!nspo_f!")
                    echo   !lang!    !mode!    !ops!    !nspo_fmt!    !total!
                ) else (
                    call :warn "!lang!/!mode!: no valid output"
                )
            ) else (
                call :warn "!lang!/!mode!: no valid output"
            )
            set "_sz="
        )
    )
)
echo.
goto :eof

REM ------------------------------------------------------------------ cond
:bench_cond
call :info "=== cond  (ping-pong, 5s) ==="
echo   LANG    MODE        ops/s        ns/op     total_ops
echo   ----------------------------------------------------

for %%L in (%LANGS:,= %) do (
    set "lang=%%L"
    set "BIN="
    set "modes="
    if /I "!lang!"=="xylem" (set "BIN=%BIN_DIR%\cond-xylem.exe" & set "modes=cc tt ct tc")
    if /I "!lang!"=="go"    (set "BIN=%BIN_DIR%\cond-go.exe"    & set "modes=cc")
    if /I "!lang!"=="rust"  (set "BIN=%BIN_DIR%\cond-rust.exe"  & set "modes=tt")
    if not defined BIN (
        call :warn "skip !lang! (cond unsupported)"
    ) else if not exist "!BIN!" (
        call :warn "skip !lang! (no binary)"
    ) else (
        for %%M in (!modes!) do (
            set "mode=%%M"
            set "out=%RUN_DIR%\sync-cond-!lang!-!mode!.json"
            "!BIN!" > "!out!" 2>nul

            set "ops="
            set "nspo="
            set "total="
            for %%F in ("!out!") do set "_sz=%%~zF"
            if defined _sz if !_sz! GTR 0 (
                for /f "tokens=1-3" %%A in ('powershell -NoProfile -Command "$m='!mode!'; $txt=gc '!out!' -Raw; $blocks=$txt -split '(?<=\})\s*\r?\n\s*(?=\{)'; foreach($b in $blocks){if($b -match ('\"mode\":\s*\"' + $m + '\"')){$o='';$n='';$t='';if($b -match '\"ops_per_sec\":\s*([0-9.]+)'){$o=$Matches[1]};if($b -match '\"ns_per_op\":\s*([0-9.]+)'){$n=$Matches[1]};if($b -match '\"total_ops\":\s*([0-9]+)'){$t=$Matches[1]};Write-Output ($o + ' ' + $n + ' ' + $t)}}"') do (
                    set "ops=%%A"
                    set "nspo=%%B"
                    set "total=%%C"
                )
                for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                if defined ops if !ops! GTR 0 (
                    call :nspo_to_x100 "!nspo!"
                    set /a nspo_i=_NSPO_X100/100
                    set /a nspo_f=_NSPO_X100%%100
                    if !nspo_f! LSS 10 (set "nspo_fmt=!nspo_i!.0!nspo_f!") else (set "nspo_fmt=!nspo_i!.!nspo_f!")
                    echo   !lang!    !mode!    !ops!    !nspo_fmt!    !total!
                ) else (
                    call :warn "!lang!/!mode!: no valid output"
                )
            ) else (
                call :warn "!lang!/!mode!: no valid output"
            )
            set "_sz="
        )
    )
)
echo.
goto :eof

REM ------------------------------------------------------------------ sem
:bench_sem
call :info "=== sem  (handoff, 5s) ==="
echo   LANG    MODE        ops/s        ns/op     total_ops
echo   ----------------------------------------------------

for %%L in (%LANGS:,= %) do (
    set "lang=%%L"
    if /I "!lang!"=="go" goto :sem_skip
    if /I "!lang!"=="xylem" (set "BIN=%BIN_DIR%\sem-xylem.exe") else (set "BIN=%BIN_DIR%\sem-rust.exe")
    if not exist "!BIN!" (
        call :warn "skip !lang! (no binary)"
    ) else (
        if /I "!lang!"=="xylem" set "modes=cc tt ct tc"
        if /I "!lang!"=="rust"  set "modes=coro"

        for %%M in (!modes!) do (
            set "mode=%%M"
            set "out=%RUN_DIR%\sync-sem-!lang!-!mode!.json"
            "!BIN!" > "!out!" 2>nul

            set "ops="
            set "nspo="
            set "total="
            for %%F in ("!out!") do set "_sz=%%~zF"
            if defined _sz if !_sz! GTR 0 (
                for /f "tokens=1-3" %%A in ('powershell -NoProfile -Command "$m='!mode!'; $txt=gc '!out!' -Raw; $blocks=$txt -split '(?<=\})\s*\r?\n\s*(?=\{)'; foreach($b in $blocks){if($b -match ('\"mode\":\s*\"' + $m + '\"')){$o='';$n='';$t='';if($b -match '\"ops_per_sec\":\s*([0-9.]+)'){$o=$Matches[1]};if($b -match '\"ns_per_op\":\s*([0-9.]+)'){$n=$Matches[1]};if($b -match '\"total_ops\":\s*([0-9]+)'){$t=$Matches[1]};Write-Output ($o + ' ' + $n + ' ' + $t)}}"') do (
                    set "ops=%%A"
                    set "nspo=%%B"
                    set "total=%%C"
                )
                for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                if defined ops if !ops! GTR 0 (
                    call :nspo_to_x100 "!nspo!"
                    set /a nspo_i=_NSPO_X100/100
                    set /a nspo_f=_NSPO_X100%%100
                    if !nspo_f! LSS 10 (set "nspo_fmt=!nspo_i!.0!nspo_f!") else (set "nspo_fmt=!nspo_i!.!nspo_f!")
                    echo   !lang!    !mode!    !ops!    !nspo_fmt!    !total!
                ) else (
                    call :warn "!lang!/!mode!: no valid output"
                )
            ) else (
                call :warn "!lang!/!mode!: no valid output"
            )
            set "_sz="
        )
    )
:sem_skip
)
echo.
goto :eof

:bench_channel
call :info "=== channel  (one-way, 5s) ==="
echo   LANG    MODE        ops/s        ns/op     total_ops
echo   ----------------------------------------------------

for %%L in (%LANGS:,= %) do (
    set "lang=%%L"
    set "BIN="
    set "modes="
    if /I "!lang!"=="xylem" (set "BIN=%BIN_DIR%\channel-xylem.exe" & set "modes=cc tt ct tc")
    if /I "!lang!"=="go"    (set "BIN=%BIN_DIR%\channel-go.exe"    & set "modes=cc")
    if /I "!lang!"=="rust"  (set "BIN=%BIN_DIR%\channel-rust.exe"  & set "modes=cc tt ct tc")
    if not defined BIN (
        call :warn "skip !lang! (channel unsupported)"
    ) else if not exist "!BIN!" (
        call :warn "skip !lang! (no binary)"
    ) else (
        for %%M in (!modes!) do (
            set "mode=%%M"
            set "out=%RUN_DIR%\sync-channel-!lang!-!mode!.json"
            "!BIN!" > "!out!" 2>nul

            set "ops="
            set "nspo="
            set "total="
            for %%F in ("!out!") do set "_sz=%%~zF"
            if defined _sz if !_sz! GTR 0 (
                for /f "tokens=1-3" %%A in ('powershell -NoProfile -Command "$m='!mode!'; $txt=gc '!out!' -Raw; $blocks=$txt -split '(?<=\})\s*\r?\n\s*(?=\{)'; foreach($b in $blocks){if($b -match ('\"mode\":\s*\"' + $m + '\"')){$o='';$n='';$t='';if($b -match '\"ops_per_sec\":\s*([0-9.]+)'){$o=$Matches[1]};if($b -match '\"ns_per_op\":\s*([0-9.]+)'){$n=$Matches[1]};if($b -match '\"total_ops\":\s*([0-9]+)'){$t=$Matches[1]};Write-Output ($o + ' ' + $n + ' ' + $t)}}"') do (
                    set "ops=%%A"
                    set "nspo=%%B"
                    set "total=%%C"
                )
                for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                if defined ops if !ops! GTR 0 (
                    call :nspo_to_x100 "!nspo!"
                    set /a nspo_i=_NSPO_X100/100
                    set /a nspo_f=_NSPO_X100%%100
                    if !nspo_f! LSS 10 (set "nspo_fmt=!nspo_i!.0!nspo_f!") else (set "nspo_fmt=!nspo_i!.!nspo_f!")
                    echo   !lang!    !mode!    !ops!    !nspo_fmt!    !total!
                ) else (
                    call :warn "!lang!/!mode!: no valid output"
                )
            ) else (
                call :warn "!lang!/!mode!: no valid output"
            )
            set "_sz="
        )
    )
)
echo.
goto :eof

:usage
echo usage: %SCRIPT_NAME% [mutex^|cond^|sem^|channel^|help]
echo.
echo Windows sync-primitive benchmark. Build auto-inits MSVC via vcvars64.bat.
echo.
echo Arguments:
echo   mutex^|cond^|sem^|channel   build + run the full comparison matrix for
echo                             that primitive (xylem vs go vs rust, all
echo                             supported modes)
echo   help                      this help
echo   (none)                    every primitive   (default)
echo.
echo The matrix is fixed: langs=xylem,go,rust, each cell runs once (no
echo repeat). Edit the defaults at the top of this script to change it.
goto :eof
