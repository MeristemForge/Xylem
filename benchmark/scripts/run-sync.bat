@echo off
setlocal EnableDelayedExpansion

REM ============================================================================
REM Xylem sync-primitive benchmark (Windows, native cmd.exe)
REM ----------------------------------------------------------------------------
REM Run from any terminal (cmd.exe / PowerShell). The build step auto-detects
REM Visual Studio via vswhere and initializes vcvars64.bat, so cl.exe is set up
REM automatically (cmake / ninja must be on PATH). See docs\build.md.
REM
REM   build  - build xylem static lib + C/Go/Rust sync-bench binaries
REM   bench  - run each primitive across xylem/go/rust, write out\results\<ts>\
REM   all    - build + bench                                         [default]
REM
REM Primitives (--prims): mutex,cond,waitgroup,sem,channel
REM Languages  (--langs): xylem,go,rust
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "BENCH_DIR=%%~fI"
set "SYNC_DIR=%BENCH_DIR%\sync"
for %%I in ("%BENCH_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "OUT_DIR=%BENCH_DIR%\out"
set "BIN_DIR=%OUT_DIR%"
set "BUILD_DIR=%OUT_DIR%\build"
set "RESULTS_ROOT=%OUT_DIR%\results"

if not defined PRIMS   set "PRIMS=mutex,cond,waitgroup,sem,channel,handoff"
if not defined LANGS   set "LANGS=xylem,go,rust"
if not defined MODES   set "MODES=coro,thread,mixed"
if not defined WORKERS set "WORKERS=0"
if not defined REPEAT  set "REPEAT=3"
if not defined PERMITS set "PERMITS=4"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=4"
set "NCPU=%NUMBER_OF_PROCESSORS%"

set "CMD=%~1"
if "%CMD%"=="" set "CMD=all"
if not "%CMD%"=="" shift

if /I "%CMD%"=="build" goto :do_build
if /I "%CMD%"=="bench" goto :do_bench
if /I "%CMD%"=="all"   goto :do_all
if /I "%CMD%"=="-h"     goto :usage
if /I "%CMD%"=="--help" goto :usage
if /I "%CMD%"=="help"   goto :usage
call :err "unknown command: %CMD%"
goto :usage

:do_build
call :parse_opts %*
call :cmd_build
goto :eof
:do_bench
call :parse_opts %*
call :cmd_bench
goto :eof
:do_all
call :parse_opts %*
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

set "CL_FLAGS=/nologo /std:c11 /O2 /DNDEBUG /MD /W3 /I\"%PROJECT_ROOT%\src\""
set "SYS_LIBS=ws2_32.lib mswsock.lib psapi.lib"

echo %LANGS% | findstr /I "xylem" >nul && (
    call :info "building xylem sync-bench..."
    if exist "%BIN_DIR%\sem-xylem.exe" del /q "%BIN_DIR%\sem-xylem.exe"
    cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SYNC_DIR%\sem\main.c" "%XYLEM_LIB%" %SYS_LIBS% /Fe:"%BIN_DIR%\sem-xylem.exe" >nul 2>&1
    if errorlevel 1 (call :err "sem-xylem build failed" & exit /b 1) else (call :ok "sem-xylem built")
)

echo %LANGS% | findstr /I "rust" >nul && (
    call :info "building sem-rust..."
    pushd "%SYNC_DIR%\sem\rust"
    cargo build --release -q --target-dir "%BIN_DIR%\cargo" >nul 2>&1 && copy /Y "%BIN_DIR%\cargo\release\sem-rust.exe" "%BIN_DIR%\" >nul && (call :ok "sem-rust built") || (call :warn "skip sem-rust")
    popd
)

echo %LANGS% | findstr /I "go" >nul && (
    if exist "%BIN_DIR%\sync-go.exe" del /q "%BIN_DIR%\sync-go.exe"
    where go >nul 2>&1
    if not errorlevel 1 (
        call :info "building go sync-bench..."
        pushd "%SYNC_DIR%\go-sync"
        set "CGO_ENABLED=0"
        go build -ldflags="-s -w" -o "%BIN_DIR%\sync-go.exe" . && (call :ok "sync-go built") || (popd & call :err "sync-go build failed" & exit /b 1)
        popd
    ) else (
        call :err "go not found"
        exit /b 1
    )
)

echo %LANGS% | findstr /I "rust" >nul && (
    if exist "%BIN_DIR%\sync-rust.exe" del /q "%BIN_DIR%\sync-rust.exe"
    where cargo >nul 2>&1
    if not errorlevel 1 (
        call :info "building rust sync-bench..."
        pushd "%SYNC_DIR%\rust-sync"
        cargo build --release -q --target-dir "%BIN_DIR%\cargo" && copy /Y "%BIN_DIR%\cargo\release\sync-rust.exe" "%BIN_DIR%\" >nul && (call :ok "sync-rust built") || (popd & call :err "sync-rust build failed" & exit /b 1)
        popd
    ) else (
        call :err "cargo not found"
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
for %%L in (%LANGS:,= %) do (
    call :bin_for %%L
    if not exist "!BIN!" (
        call :err "binary for %%L missing; run: %~nx0 build"
        exit /b 1
    )
)

for /f "tokens=2 delims==" %%T in ('wmic os get localdatetime /value 2^>nul ^| find "="') do set "_DT=%%T"
set "TS=%_DT:~0,8%-%_DT:~8,6%"
set "RUN_DIR=%RESULTS_ROOT%\%TS%"
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"

call :info "results -> %RUN_DIR%   workers=%WORKERS% repeat=%REPEAT%"
call :info "prims: %PRIMS%   langs: %LANGS%"
echo.

for %%P in (%PRIMS:,= %) do call :bench_prim %%P

call :ok "sync benchmarks complete"
call :info "results written to %RUN_DIR%"
goto :eof

REM bin_for <lang> -> BIN
:bin_for
if /I "%~1"=="xylem" set "BIN=%BIN_DIR%\sync-xylem.exe"
if /I "%~1"=="go"    set "BIN=%BIN_DIR%\sync-go.exe"
if /I "%~1"=="rust"  set "BIN=%BIN_DIR%\sync-rust.exe"
goto :eof

REM prim_params <prim> -> TASKS / ITERS (coro) / ITERS_T (thread+mixed, lighter)
:prim_params
set "TASKS=8"
set "ITERS=1000000"
set "ITERS_T=1000000"
if /I "%~1"=="mutex"     (set "TASKS=8" & set "ITERS=1000000" & set "ITERS_T=1000000")
if /I "%~1"=="cond"      (set "TASKS=2" & set "ITERS=2000000" & set "ITERS_T=2000000")
if /I "%~1"=="waitgroup" (set "TASKS=8" & set "ITERS=50000"  & set "ITERS_T=2000")
if /I "%~1"=="channel"   (set "TASKS=4" & set "ITERS=1000000" & set "ITERS_T=1000000")
if /I "%~1"=="handoff"   (set "TASKS=2" & set "ITERS=500000"  & set "ITERS_T=500000")
goto :eof

REM supported <lang> <mode> <prim> -> _SUP=1/0
:supported
set "_SUP=0"
if /I "%~1"=="go"    ( if /I "%~2"=="coro" set "_SUP=1" )
if /I "%~1"=="rust"  ( if /I "%~2"=="coro" set "_SUP=1"
                       if /I "%~2"=="thread" set "_SUP=1"
                       if /I "%~2"=="mixed" ( if /I "%~3"=="channel" set "_SUP=1" )
                       if /I "%~3"=="handoff" set "_SUP=1" )
if /I "%~1"=="xylem" set "_SUP=1"
goto :eof

REM bench_prim <prim>
:bench_prim
set "PRIM=%~1"
if /I "%PRIM%"=="sem" (
    call :bench_sem
    goto :eof
)
call :prim_params "%PRIM%"
call :info "=== %PRIM%  (tasks=%TASKS%) ==="
echo   LANG    MODE      ops/s(avg)        ns/op      total_ops  runs(ops/s)
echo   -------------------------------------------------------------------------------

for %%L in (%LANGS:,= %) do (
    set "lang=%%L"
    call :bin_for !lang!
    if not exist "!BIN!" (
        call :warn "skip !lang! (no binary)"
    ) else (
        for %%M in (%MODES:,= %) do (
            set "mode=%%M"
            call :supported !lang! !mode! %PRIM%
            if "!_SUP!"=="1" (
                set "ITERS_USE=%ITERS%"
                if /I not "!mode!"=="coro" set "ITERS_USE=%ITERS_T%"
                set "EXTRA="
                if /I "%PRIM%"=="sem" set "EXTRA=--permits %PERMITS%"

                set /a ops_sum=0, nspo_sum=0, valid=0
                set "ops_vals="
                set "nspo_avg=0.00"
                set "total_last=0"

                for /l %%R in (1,1,%REPEAT%) do (
                    set "out=%RUN_DIR%\sync-%PRIM%-!lang!-!mode!-r%%R.json"
                    "!BIN!" %PRIM% --mode !mode! --workers %WORKERS% --tasks %TASKS% --iters !ITERS_USE! !EXTRA! > "!out!" 2>nul

                    for %%F in ("!out!") do set "_sz=%%~zF"
                    if defined _sz if !_sz! GTR 0 (
                        call :extract_json "!out!" ops_per_sec
                        set "ops=!_JVAL!"
                        call :extract_json "!out!" ns_per_op
                        set "nspo=!_JVAL!"
                        call :extract_json "!out!" total_ops
                        set "total=!_JVAL!"
                        call :extract_json_string "!out!" mode
                        set "reported_mode=!_JVAL!"
                        if defined reported_mode if /I not "!reported_mode!"=="!mode!" (
                            set "renamed=%RUN_DIR%\sync-%PRIM%-!lang!-!reported_mode!-r%%R.json"
                            move /Y "!out!" "!renamed!" >nul
                            set "out=!renamed!"
                        )
                        for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                        if defined ops if !ops! GTR 0 (
                            set /a ops_sum+=ops, valid+=1
                            call :nspo_to_x100 "!nspo!"
                            set /a nspo_sum+=_NSPO_X100
                            set "total_last=!total!"
                            if defined ops_vals (set "ops_vals=!ops_vals!,!ops!") else (set "ops_vals=!ops!")
                        )
                    )
                    set "_sz="
                )

                if !valid! GTR 0 (
                    set /a ops_avg=ops_sum/valid
                    set /a nspo_avg_x100=nspo_sum/valid
                    set /a nspo_avg_i=nspo_avg_x100/100
                    set /a nspo_avg_f=nspo_avg_x100%%100
                    if !nspo_avg_f! LSS 10 (set "nspo_avg=!nspo_avg_i!.0!nspo_avg_f!") else (set "nspo_avg=!nspo_avg_i!.!nspo_avg_f!")
                    echo   !lang!    !mode!    !ops_avg!    !nspo_avg!    !total_last!  [!ops_vals!]
                ) else (
                    call :warn "!lang!/!mode!: no valid output from %REPEAT% runs"
                )
            )
        )
    )
)
echo.
goto :eof

REM extract_json <file> <key> -> _JVAL
:extract_json
set "_JVAL="
for /f "tokens=2 delims=:" %%A in ('findstr /C:"\"%~2\"" "%~1" 2^>nul') do set "_raw=%%A"
if defined _raw (
    set "_raw=!_raw: =!"
    set "_raw=!_raw:,=!"
    set "_JVAL=!_raw!"
)
set "_raw="
goto :eof

REM extract_json_string <file> <key> -> _JVAL
:extract_json_string
set "_JVAL="
for /f "tokens=2 delims=:" %%A in ('findstr /C:"\"%~2\"" "%~1" 2^>nul') do set "_raw=%%A"
if defined _raw (
    set "_raw=!_raw: =!"
    set "_raw=!_raw:,=!"
    set "_raw=!_raw:"=!"
    set "_JVAL=!_raw!"
)
set "_raw="
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

REM ----------------------------------------------------------- option parse
:parse_opts
if "%~1"=="" goto :eof
set "_opt=%~1"
if /I "%_opt%"=="build" (shift & goto :parse_opts)
if /I "%_opt%"=="bench" (shift & goto :parse_opts)
if /I "%_opt%"=="all"   (shift & goto :parse_opts)
if /I "%_opt%"=="--prims"   (set "PRIMS=" & shift & set "_LV=PRIMS" & goto :collect_list)
if /I "%_opt%"=="-p"        (set "PRIMS=" & shift & set "_LV=PRIMS" & goto :collect_list)
if /I "%_opt%"=="--langs"   (set "LANGS=" & shift & set "_LV=LANGS" & goto :collect_list)
if /I "%_opt%"=="-l"        (set "LANGS=" & shift & set "_LV=LANGS" & goto :collect_list)
if /I "%_opt%"=="--modes"   (set "MODES=" & shift & set "_LV=MODES" & goto :collect_list)
if /I "%_opt%"=="-m"        (set "MODES=" & shift & set "_LV=MODES" & goto :collect_list)
if /I "%_opt%"=="--workers" (set "WORKERS=%~2" & shift & shift & goto :parse_opts)
if /I "%_opt%"=="-w"        (set "WORKERS=%~2" & shift & shift & goto :parse_opts)
if /I "%_opt%"=="--repeat"  (set "REPEAT=%~2" & shift & shift & goto :parse_opts)
if /I "%_opt%"=="-r"        (set "REPEAT=%~2" & shift & shift & goto :parse_opts)
if /I "%_opt%"=="--permits" (set "PERMITS=%~2" & shift & shift & goto :parse_opts)
call :err "unknown option: %_opt%"
exit /b 1

REM Collect a comma/space-separated list into the variable named by _LV,
REM consuming value tokens until the next option (starts with '-') or end of
REM args. cmd splits an unquoted `xylem,rust` into separate tokens on the
REM comma, so we rejoin them here -- this makes `--langs xylem,rust`,
REM `--langs xylem rust`, and a quoted "xylem,rust" all work.
:collect_list
if "%~1"=="" goto :parse_opts
set "_lt=%~1"
if "%_lt:~0,1%"=="-" goto :parse_opts
call set "_cur=%%%_LV%%%"
if defined _cur (call set "%_LV%=%%%_LV%%%,%_lt%") else (set "%_LV%=%_lt%")
shift
goto :collect_list

REM ------------------------------------------------------------------ sem
:bench_sem
call :info "=== sem  (handoff, 5s) ==="
echo   LANG    MODE      ops/s(avg)        ns/op      total_ops  runs(ops/s^)
echo   -------------------------------------------------------------------------------

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
            set /a ops_sum=0, nspo_sum=0, valid=0
            set "ops_vals="
            set "total_last=0"

            for /l %%R in (1,1,%REPEAT%) do (
                set "out=%RUN_DIR%\sync-sem-!lang!-!mode!-r%%R.json"
                if /I "!lang!"=="xylem" (
                    "%BIN_DIR%\sem-xylem.exe" > "!out!" 2>nul
                ) else (
                    "%BIN_DIR%\sem-rust.exe" > "!out!" 2>nul
                )

                for %%F in ("!out!") do set "_sz=%%~zF"
                if defined _sz if !_sz! GTR 0 (
                    for /f "tokens=1-3 delims=|" %%A in ('powershell -NoProfile -Command "$m='!mode!'; $txt=gc '!out!' -Raw; $blocks=$txt -split '(?<=\})\s*\r?\n\s*(?=\{)'; foreach($b in $blocks){if($b -match \"`\"mode`\":\s*`\"$m`\"\"){$o='';$n='';$t='';if($b -match '`\"ops_per_sec`\":\s*([0-9.]+)'){$o=$Matches[1]};if($b -match '`\"ns_per_op`\":\s*([0-9.]+)'){$n=$Matches[1]};if($b -match '`\"total_ops`\":\s*([0-9]+)'){$t=$Matches[1]};Write-Output \"$o|$n|$t\"}}"') do (
                        set "ops=%%A"
                        set "nspo=%%B"
                        set "total=%%C"
                    )
                    for /f "delims=." %%X in ("!ops!") do set "ops=%%X"
                    if defined ops if !ops! GTR 0 (
                        set /a ops_sum+=ops, valid+=1
                        call :nspo_to_x100 "!nspo!"
                        set /a nspo_sum+=_NSPO_X100
                        set "total_last=!total!"
                        if defined ops_vals (set "ops_vals=!ops_vals!,!ops!") else (set "ops_vals=!ops!")
                    )
                )
                set "_sz="
            )

            if !valid! GTR 0 (
                set /a ops_avg=ops_sum/valid
                set /a nspo_avg_x100=nspo_sum/valid
                set /a nspo_avg_i=nspo_avg_x100/100
                set /a nspo_avg_f=nspo_avg_x100%%100
                if !nspo_avg_f! LSS 10 (set "nspo_avg=!nspo_avg_i!.0!nspo_avg_f!") else (set "nspo_avg=!nspo_avg_i!.!nspo_avg_f!")
                echo   !lang!    !mode!    !ops_avg!    !nspo_avg!    !total_last!  [!ops_vals!]
            ) else (
                call :warn "!lang!/!mode!: no valid output from %REPEAT% runs"
            )
        )
    )
    :sem_skip
)
echo.
goto :eof

:usage
echo usage: %~nx0 [build^|bench^|all] [options]
echo.
echo Windows sync-primitive benchmark. Build auto-inits MSVC via vcvars64.bat.
echo.
echo Commands:
echo   build   build xylem static lib + C/Go/Rust sync-bench binaries
echo   bench   run each primitive across languages, write out\results\^<ts^>\
echo   all     build + bench   (default)
echo.
echo Options:
echo   --prims, -p    mutex,cond,waitgroup,sem,channel   primitives to run
echo   --langs, -l    xylem,go,rust                       languages to compare
echo   --modes, -m    coro,thread,mixed                   concurrency models
echo                                                      (go: coro; rust: coro,thread,+channel/handoff mixed)
echo   --workers, -w  0                                   runtime worker threads
echo   --repeat, -r   3                                   repeat each cell N times
echo   --permits      4                                   semaphore permits (sem)
echo.
echo Examples:
echo   %~nx0
echo   %~nx0 bench --prims mutex,channel --langs xylem,rust --workers 4
goto :eof
