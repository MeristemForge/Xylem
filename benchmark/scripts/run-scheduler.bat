@echo off
setlocal EnableDelayedExpansion

REM Xylem scheduler spawn benchmark (Windows, native cmd.exe)
REM The benchmark executables have no command-line parameters. The workload is
REM fixed at one million tasks in each ST/MT source file.

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_NAME=%~nx0"
for %%I in ("%SCRIPT_DIR%..") do set "BENCH_DIR=%%~fI"
for %%I in ("%BENCH_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "SPAWN_DIR=%BENCH_DIR%\scheduler\spawn"
set "OUT_DIR=%BENCH_DIR%\out"
set "BIN_DIR=%OUT_DIR%"
set "BUILD_DIR=%OUT_DIR%\scheduler-build"
set "RESULTS_ROOT=%OUT_DIR%\results"

if not defined LANGS set "LANGS=xylem,go,rust"
if not defined REPEAT set "REPEAT=3"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=all"
if not "%~1"=="" shift

if /I "%CMD%"=="build" goto :do_build
if /I "%CMD%"=="bench" goto :do_bench
if /I "%CMD%"=="all" goto :do_all
if /I "%CMD%"=="help" goto :usage
if /I "%CMD%"=="-h" goto :usage
if /I "%CMD%"=="--help" goto :usage
call :err "unknown command: %CMD%"
goto :usage

:do_build
call :parse_opts %*
call :validate_opts || exit /b 1
call :cmd_build
goto :eof

:do_bench
call :parse_opts %*
call :validate_opts || exit /b 1
call :cmd_bench
goto :eof

:do_all
call :parse_opts %*
call :validate_opts || exit /b 1
call :cmd_build || exit /b 1
call :cmd_bench
goto :eof

:info
echo [scheduler] %~1
goto :eof
:ok
echo [ok] %~1
goto :eof
:err
echo [err] %~1 1>&2
goto :eof

:ensure_msvc
where cl >nul 2>&1 && goto :eof
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    call :err "MSVC not found and vswhere is missing"
    exit /b 1
)
set "VSPATH="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%I"
if not defined VSPATH (
    call :err "no Visual Studio installation found"
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>&1 || (call :err "cl.exe unavailable after vcvars64.bat" & exit /b 1)
goto :eof

:cmd_build
call :ensure_msvc || exit /b 1
where cmake >nul 2>&1 || (call :err "cmake not found" & exit /b 1)
where ninja >nul 2>&1 || (call :err "ninja not found" & exit /b 1)
where python >nul 2>&1 || (call :err "python not found" & exit /b 1)
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

call :info "building xylem static library"
cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DXYLEM_ENABLE_TLS=OFF || exit /b 1
cmake --build "%BUILD_DIR%" --target xylem -j %NUMBER_OF_PROCESSORS% || exit /b 1
set "XYLEM_LIB="
for /r "%BUILD_DIR%" %%F in (xylem.lib) do if not defined XYLEM_LIB set "XYLEM_LIB=%%F"
if not defined XYLEM_LIB (call :err "xylem.lib not found" & exit /b 1)

echo %LANGS% | findstr /I "xylem" >nul && (
    call :info "building xylem ST"
    cl /nologo /std:c11 /experimental:c11atomics /O2 /DNDEBUG /MD /W3 /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SPAWN_DIR%\xylem\spawn.c" "%XYLEM_LIB%" ws2_32.lib mswsock.lib psapi.lib /Fe:"%BIN_DIR%\spawn-xylem.exe" || exit /b 1
    call :info "building xylem MT"
    cl /nologo /std:c11 /experimental:c11atomics /O2 /DNDEBUG /MD /W3 /I"%PROJECT_ROOT%\include" /I"%PROJECT_ROOT%\src" "%SPAWN_DIR%\xylem\spawn-mt.c" "%XYLEM_LIB%" ws2_32.lib mswsock.lib psapi.lib /Fe:"%BIN_DIR%\spawn-xylem-mt.exe" || exit /b 1
)

echo %LANGS% | findstr /I "go" >nul && (
    where go >nul 2>&1 || (call :err "go not found" & exit /b 1)
    call :info "building go scheduler benchmarks"
    pushd "%SPAWN_DIR%\go"
    set "CGO_ENABLED=0"
    go build -trimpath -ldflags="-s -w" -o "%BIN_DIR%\spawn-go.exe" .\spawn || (popd & exit /b 1)
    go build -trimpath -ldflags="-s -w" -o "%BIN_DIR%\spawn-go-mt.exe" .\spawn-mt || (popd & exit /b 1)
    popd
)

echo %LANGS% | findstr /I "rust" >nul && (
    where cargo >nul 2>&1 || (call :err "cargo not found" & exit /b 1)
    call :info "building rust scheduler benchmarks"
    pushd "%SPAWN_DIR%\rust"
    cargo build --release --bins -q --target-dir "%BIN_DIR%\cargo" || (popd & exit /b 1)
    copy /Y "%BIN_DIR%\cargo\release\spawn-rust.exe" "%BIN_DIR%\" >nul || (popd & exit /b 1)
    copy /Y "%BIN_DIR%\cargo\release\spawn-rust-mt.exe" "%BIN_DIR%\" >nul || (popd & exit /b 1)
    popd
)
call :ok "scheduler binaries built"
exit /b 0

:cmd_bench
where python >nul 2>&1 || (call :err "python not found" & exit /b 1)
for /f "delims=" %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%T"
set "RUN_DIR=%RESULTS_ROOT%\%TS%"
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"
call :info "results: %RUN_DIR% (repeat=%REPEAT%)"

for %%L in (%LANGS:,= %) do (
    for %%M in (st mt) do (
        set "BIN="
        if /I "%%L"=="xylem" if /I "%%M"=="st" set "BIN=%BIN_DIR%\spawn-xylem.exe"
        if /I "%%L"=="xylem" if /I "%%M"=="mt" set "BIN=%BIN_DIR%\spawn-xylem-mt.exe"
        if /I "%%L"=="go" if /I "%%M"=="st" set "BIN=%BIN_DIR%\spawn-go.exe"
        if /I "%%L"=="go" if /I "%%M"=="mt" set "BIN=%BIN_DIR%\spawn-go-mt.exe"
        if /I "%%L"=="rust" if /I "%%M"=="st" set "BIN=%BIN_DIR%\spawn-rust.exe"
        if /I "%%L"=="rust" if /I "%%M"=="mt" set "BIN=%BIN_DIR%\spawn-rust-mt.exe"
        if not exist "!BIN!" (call :err "missing binary: !BIN!" & exit /b 1)
        for /l %%R in (1,1,%REPEAT%) do (
            set "RESULT=%RUN_DIR%\scheduler-spawn-%%L-%%M-r%%R.json"
            "!BIN!" > "!RESULT!"
            call :verify_result "!RESULT!" %%L %%M
            if errorlevel 1 exit /b 1
        )
        call :print_summary "%RUN_DIR%" %%L %%M
    )
)
call :ok "scheduler benchmarks complete"
exit /b 0

:print_summary
set "SUMMARY="
for /f "tokens=1-3" %%A in ('python -c "import json,os,sys; d,l,m,n=sys.argv[1:]; a=[json.load(open(os.path.join(d,'scheduler-spawn-'+l+'-'+m+'-r'+str(i)+'.json'))) for i in range(1,int(n)+1)]; print('%%.6f %%.0f %%.2f' %% (sum(x['elapsed_sec'] for x in a)/len(a),sum(x['tasks_per_sec'] for x in a)/len(a),sum(x['ns_per_task'] for x in a)/len(a)))" "%~1" "%~2" "%~3" "%REPEAT%"') do (
    set "SUMMARY=%%A %%B %%C"
)
if not defined SUMMARY (call :err "failed to summarize scheduler results" & exit /b 1)
echo %2 %3 elapsed/tasks-per-sec/ns-per-task: !SUMMARY!
goto :eof

:verify_result
set "_VERIFY_FILE=%~1"
set "_VERIFY_LANG=%~2"
set "_VERIFY_MODE=%~3"
python -c "import json,math,os,sys; p,l,m=sys.argv[1:]; r=json.load(open(p)); t=1000000; e={'benchmark':'spawn','lang':l,'mode':m,'tasks':t,'completed':t}; bad=[k for k,v in e.items() if not r.get(k) == v]; w=1 if m=='st' else (os.cpu_count() or 1); bad += ['workers'] if not r.get('workers') == w or not isinstance(r.get('workers'),int) or isinstance(r.get('workers'),bool) or r.get('workers',0)<1 else []; bad += [k for k in ('elapsed_sec','tasks_per_sec','ns_per_task') if not isinstance(r.get(k),(int,float)) or isinstance(r.get(k),bool) or not math.isfinite(r.get(k)) or r.get(k)<=0]; sys.exit('invalid scheduler result: '+','.join(bad)) if bad else None" "%_VERIFY_FILE%" "%_VERIFY_LANG%" "%_VERIFY_MODE%"
if errorlevel 1 exit /b 1
goto :eof

:parse_opts
if "%~1"=="" (
    set "READ_LANGS="
    goto :eof
)
if defined READ_LANGS (
    set "_LANG_TOKEN=%~1"
    if "!_LANG_TOKEN:~0,1!"=="-" (
        set "READ_LANGS="
        goto :parse_opts
    )
    if defined LANGS (set "LANGS=!LANGS!,%~1") else (set "LANGS=%~1")
    shift
    goto :parse_opts
)
if /I "%~1"=="build" (
    shift
    goto :parse_opts
)
if /I "%~1"=="bench" (
    shift
    goto :parse_opts
)
if /I "%~1"=="all" (
    shift
    goto :parse_opts
)
if /I "%~1"=="--langs" (
    set "LANGS="
    set "READ_LANGS=1"
    shift
    goto :parse_opts
)
if /I "%~1"=="-l" (
    set "LANGS="
    set "READ_LANGS=1"
    shift
    goto :parse_opts
)
if /I "%~1"=="--repeat" (
    set "REPEAT=%~2"
    shift
    shift
    goto :parse_opts
)
if /I "%~1"=="-r" (
    set "REPEAT=%~2"
    shift
    shift
    goto :parse_opts
)
call :err "unknown option: %~1"
exit /b 1

:validate_opts
if not defined REPEAT (call :err "repeat must be a positive integer" & exit /b 1)
for /f "delims=0123456789" %%A in ("%REPEAT%") do if not "%%A"=="" (call :err "repeat must be a positive integer" & exit /b 1)
if %REPEAT% LEQ 0 (call :err "repeat must be a positive integer" & exit /b 1)
for %%L in (%LANGS:,= %) do (
    if /I not "%%L"=="xylem" if /I not "%%L"=="go" if /I not "%%L"=="rust" (
        call :err "unsupported language: %%L"
        exit /b 1
    )
)
goto :eof

:usage
echo usage: %SCRIPT_NAME% [build^|bench^|all] [options...]
echo.
echo Commands:
echo   build             build the six scheduler benchmark binaries
echo   bench             run both ST and MT modes and write results/%%timestamp%%/
echo   all               build + bench (default)
echo.
echo Options:
echo   --langs, -l LIST  comma-separated subset of xylem,go,rust
echo   --repeat, -r N    runs per language/mode (default: 3)
echo.
echo The workload is fixed at 1,000,000 tasks. Benchmark executables take no args.
exit /b 0
