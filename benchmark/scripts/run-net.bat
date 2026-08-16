@echo off
setlocal EnableDelayedExpansion

REM ============================================================================
REM Xylem benchmark suite (Windows, native cmd.exe)
REM ----------------------------------------------------------------------------
REM Usage:  run-net.bat <proto>   - build + run the full comparison matrix
REM                                  for one protocol, e.g.:
REM     run-net.bat tcp     TCP:   stream echo,  ports from 9000,
REM                                 ST + MT, throughput + connrate
REM     run-net.bat udp     UDP:   datagram echo, ports from 9001,
REM                                 ST only, throughput
REM     run-net.bat tls     TLS:   TLS-over-TCP, ports from 9443,
REM                                 ST + MT, throughput + connrate
REM                                 (xylem built -DXYLEM_ENABLE_TLS=ON;
REM                                  needs OpenSSL via vcpkg)
REM     run-net.bat              - same, all protocols (tcp,udp,tls)
REM     run-net.bat help         - usage
REM
REM Missing toolchain pieces print setup guidance automatically when the run
REM starts (cl.exe is auto-initialized by the build step via vcvars64.bat).
REM
REM Run from any terminal (cmd.exe / PowerShell). The build step auto-detects
REM Visual Studio via vswhere and initializes vcvars64.bat, so cl.exe is set up
REM automatically; running from a "Developer Command Prompt" also works. cmake
REM and ninja must be on PATH (winget installs put them there).
REM
REM Compared servers: xylem, go, rust. The Windows driver builds only these
REM three families for every protocol; missing binaries are skipped
REM automatically. UDP has no MT row.
REM
REM NOTE: the bench clients are Go programs (net/<proto>/client); the Go
REM runtime netpoller uses IOCP on Windows. Windows lacks
REM SO_REUSEPORT / /proc; per-CPU usage sampling is unavailable and numbers are
REM NOT comparable to the Linux or macOS suites. Server peak working-set is
REM reported via PowerShell Get-Process.
REM ============================================================================

REM ---- paths -----------------------------------------------------------------
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_PATH=%~f0"
set "SCRIPT_NAME=%~nx0"
for %%I in ("%SCRIPT_DIR%..") do set "BENCH_DIR=%%~fI"
for %%I in ("%BENCH_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "NET_DIR=%BENCH_DIR%\net"
set "OUT_DIR=%BENCH_DIR%\out"
set "BIN_DIR=%OUT_DIR%"
set "BUILD_DIR=%OUT_DIR%\build"
set "RESULTS_ROOT=%OUT_DIR%\results"
set "WINCLIENT_PS1=%OUT_DIR%\run-net-winclient.ps1"

REM ---- fixed benchmark matrix ------------------------------------------------
REM Keep the benchmark matrix in one place. The driver intentionally does not
REM accept CLI flags for these values; edit these constants when changing the
REM standard suite.
set "NET_BENCH_PROTO=tcp,udp,tls"
set "NET_BENCH_SERVERS=xylem,go,rust"
set "NET_BENCH_CONNS=1000,10000"
set "NET_BENCH_PAYLOADS=64,4096,65536"
set "NET_BENCH_DURATION=10"
set "NET_BENCH_MODE=both"
set "NET_BENCH_WARMUP_RUNS=1"
set "NET_BENCH_RUN_CONNRATE=true"

set "PROTO=%NET_BENCH_PROTO%"
set "SERVERS=%NET_BENCH_SERVERS%"
set "CONNS=%NET_BENCH_CONNS%"
set "PAYLOADS=%NET_BENCH_PAYLOADS%"
set "DURATION=%NET_BENCH_DURATION%"
set "MODE=%NET_BENCH_MODE%"
set "BENCH_WARMUP_RUNS=%NET_BENCH_WARMUP_RUNS%"
set "RUN_CONNRATE=%NET_BENCH_RUN_CONNRATE%"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=4"
set "NCPU=%NUMBER_OF_PROCESSORS%"

REM ---- core pinning (single-host fairness) -----------------------------------
REM Ping-pong echo is symmetric, so split cores 50/50: the server runs on the
REM low cores, the client on the high cores, via start /affinity. Auto-on with
REM >=4 cores; PIN=off disables, PIN=on forces.
if not defined PIN set "PIN=auto"
set "PIN_ENABLE=false"
if /I not "%PIN%"=="off" if %NCPU% GEQ 4 set "PIN_ENABLE=true"
if /I "%PIN%"=="on" set "PIN_ENABLE=true"
set "SERVER_NCPU=%NCPU%"
set "CLIENT_NCPU=%NCPU%"
set "SRV_MASK="
set "CLI_MASK="
set "SRV_ST_MASK=1"
if "%PIN_ENABLE%"=="true" (
    for /f "tokens=1-4" %%a in ('powershell -NoProfile -Command "$n=%NCPU%; $s=[int]($n/2); if($s -lt 1){$s=1}; $c=$n-$s; $sm=([int64]1 -shl $s)-1; $full=([int64]1 -shl $n)-1; $cm=$full -band (-bnot $sm); '{0:X} {1:X} {2} {3}' -f $sm,$cm,$s,$c"') do (
        set "SRV_MASK=%%a"
        set "CLI_MASK=%%b"
        set "SERVER_NCPU=%%c"
        set "CLIENT_NCPU=%%d"
    )
)

REM ---- dispatch --------------------------------------------------------------
REM One argument selects the protocol: run-net.bat tcp|udp|tls builds and runs
REM the full comparison matrix for that protocol. No argument runs all
REM protocols. Missing toolchain pieces print setup guidance automatically.
set "CMD=%~1"
if not "%CMD%"=="" shift

if /I "%CMD%"=="tcp"    goto :do_proto
if /I "%CMD%"=="udp"    goto :do_proto
if /I "%CMD%"=="tls"    goto :do_proto
if /I "%CMD%"=="-h"     goto :usage
if /I "%CMD%"=="--help" goto :usage
if /I "%CMD%"=="help"   goto :usage
if not "%CMD%"=="" (
    call :err "unknown target: %CMD% (must be tcp|udp|tls|help)"
    goto :usage
)

:do_proto
if not "%~1"=="" (
    call :err "unexpected extra arguments: %*"
    goto :usage
)
if not "%CMD%"=="" set "PROTO=%CMD%"
call :ensure_deps || exit /b 1
call :cmd_build || exit /b 1
call :cmd_bench
goto :eof

REM ============================================================================
REM logging helpers
REM ============================================================================
:info
echo [bench] %~1
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

REM ============================================================================
REM per-protocol config: sets PORT_BASE / HAS_MT / HAS_CONNRATE / IS_TLS / PROTO_CONNS
REM ============================================================================
:proto_config
set "_p=%~1"
if /I "%_p%"=="tcp" (
    set "PORT_BASE=9000"
    set "HAS_MT=true"
    set "HAS_CONNRATE=true"
    set "IS_TLS=false"
    set "PROTO_CONNS=1000,10000"
    set "PROTO_PAYLOADS=64,4096,65536"
    exit /b 0
)
if /I "%_p%"=="udp" (
    set "PORT_BASE=9001"
    set "HAS_MT=false"
    set "HAS_CONNRATE=false"
    set "IS_TLS=false"
    set "PROTO_CONNS=1"
    set "PROTO_PAYLOADS=64,1400"
    exit /b 0
)
if /I "%_p%"=="tls" (
    set "PORT_BASE=9443"
    set "HAS_MT=true"
    set "HAS_CONNRATE=true"
    set "IS_TLS=true"
    set "PROTO_CONNS=1000,10000"
    set "PROTO_PAYLOADS=64,4096,65536"
    exit /b 0
)
call :err "unknown protocol: %_p% (must be tcp|udp|tls)"
exit /b 1

REM ============================================================================
REM dependencies (guidance printed automatically when tools are missing)
REM ============================================================================

REM ensure_deps -- warn on optional (go/rust) gaps, print guidance + abort on
REM required (cmake/ninja) gaps. cl.exe is handled by ensure_msvc at build time.
:ensure_deps
set "_MISSING="
where cmake >nul 2>&1 || set "_MISSING=%_MISSING% cmake"
where ninja >nul 2>&1 || set "_MISSING=%_MISSING% ninja"
if not "%_MISSING%"=="" (
    call :err "required tools missing:%_MISSING%"
    call :cmd_install
    exit /b 1
)
where go >nul 2>&1 || call :warn "go not found; go server rows will be skipped"
where cargo >nul 2>&1 || call :warn "rust not found; rust server rows will be skipped"
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
echo   For TLS, OpenSSL via vcpkg:
echo     git clone https://github.com/microsoft/vcpkg
echo     .\vcpkg\bootstrap-vcpkg.bat
echo     .\vcpkg\vcpkg install openssl
echo.
where cl >nul 2>&1
if errorlevel 1 (
    call :warn "cl.exe not on PATH now; build will auto-init MSVC via vcvars64.bat."
) else (
    call :ok "cl.exe detected"
)
goto :eof

REM ============================================================================
REM ensure MSVC (cl.exe) is available -- auto-init via vcvars64.bat so the
REM script need not be launched from a "Developer Command Prompt".
REM ============================================================================
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
call "%_VCVARS%" >nul 2>&1
where cl >nul 2>&1 || (call :err "cl.exe still not found after vcvars64.bat" & exit /b 1)
goto :eof

REM ============================================================================
REM build
REM ============================================================================
:cmd_build
call :ensure_msvc || exit /b 1
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

REM xylem TLS flag = ON if tls is among the requested protocols
set "TLS_FLAG=OFF"
for %%P in (%PROTO:,= %) do if /I "%%P"=="tls" set "TLS_FLAG=ON"

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

call :info "building xylem static library (XYLEM_ENABLE_TLS=%TLS_FLAG%)..."
if "%USE_NINJA%"=="true" (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl "-DCMAKE_C_FLAGS_RELEASE=/O2 /DNDEBUG /GL" -DXYLEM_ENABLE_TLS=%TLS_FLAG% >nul 2>&1
    cmake --build "%BUILD_DIR%" --target xylem -j %NCPU% >nul 2>&1
) else (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -DCMAKE_C_COMPILER=cl "-DCMAKE_C_FLAGS_RELEASE=/O2 /DNDEBUG /GL" -DXYLEM_ENABLE_TLS=%TLS_FLAG% >nul 2>&1
    cmake --build "%BUILD_DIR%" --target xylem --config Release -j %NCPU% >nul 2>&1
)

call :find_xylem_lib
if not defined XYLEM_LIB (
    call :err "could not locate built xylem static library under %BUILD_DIR%"
    exit /b 1
)
if not exist "%XYLEM_LIB%" (
    call :err "could not locate built xylem static library under %BUILD_DIR%"
    exit /b 1
)
call :ok "xylem built (%XYLEM_LIB%)"

set "CL_FLAGS=/nologo /std:c11 /O2 /DNDEBUG /GL /MD /W3"
set "SYS_LIBS=ws2_32.lib mswsock.lib psapi.lib"

for %%P in (%PROTO:,= %) do call :build_proto "%%P"
echo.
dir /a-d "%BIN_DIR%"
goto :eof

REM build_proto <proto>
:build_proto
set "CUR_PROTO=%~1"
call :proto_config "%CUR_PROTO%"
if errorlevel 1 (
    call :err "proto_config failed for %CUR_PROTO%"
    goto :eof
)

set "EXTRA_LIBS="
if "%IS_TLS%"=="true" set "EXTRA_LIBS=libssl.lib libcrypto.lib"

REM ---- xylem echo servers (ST always; MT only if protocol has it) -----------
call :build_xylem_server "" 
if "%HAS_MT%"=="true" call :build_xylem_server "-mt"

REM ---- go servers -----------------------------------------------------------
where go >nul 2>&1
if not errorlevel 1 (
    call :build_go_server ""
    if "%HAS_MT%"=="true" call :build_go_server "-mt"
) else (
    call :warn "go not found; skipping go servers"
)

REM ---- rust servers ---------------------------------------------------------
where cargo >nul 2>&1
if not errorlevel 1 (
    call :build_rust_server ""
    if "%HAS_MT%"=="true" call :build_rust_server "-mt"
) else (
    call :warn "cargo not found; skipping rust servers"
)

REM ---- bench client (all protocols: Go multi-core load generator) ----------
call :info "building %CUR_PROTO%-bench client..."
if exist "%BIN_DIR%\%CUR_PROTO%-bench.exe" del /q "%BIN_DIR%\%CUR_PROTO%-bench.exe" >nul 2>&1
set "CGO_ENABLED=0"
pushd "%NET_DIR%\%CUR_PROTO%\client"
go build -ldflags="-s -w" -o "%BIN_DIR%\%CUR_PROTO%-bench.exe" .
popd
if not exist "%BIN_DIR%\%CUR_PROTO%-bench.exe" (
    call :err "failed to build %CUR_PROTO%-bench client"
    exit /b 1
)
call :ok "%CUR_PROTO%-bench built"
goto :eof

REM build_xylem_server <suffix>
:build_xylem_server
set "_SUF=%~1"
set "_SRC=%NET_DIR%\%CUR_PROTO%\server\xylem-echo\server%_SUF%.c"
set "_OUT=%BIN_DIR%\%CUR_PROTO%-xylem-echo%_SUF%.exe"
if not exist "%_SRC%" goto :eof
call :info "building %CUR_PROTO%-xylem-echo%_SUF%..."
cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" "%_SRC%" "%XYLEM_LIB%" %SYS_LIBS% %EXTRA_LIBS% /Fe:"%_OUT%" /link /LTCG >nul 2>&1
if errorlevel 1 (
    call :warn "skip xylem %CUR_PROTO%%_SUF% (build failed)"
) else (
    call :ok "%CUR_PROTO%-xylem-echo%_SUF% built"
)
goto :eof

REM build_go_server <suffix>
:build_go_server
set "_SUF=%~1"
set "_DIR=%NET_DIR%\%CUR_PROTO%\server\go-echo"
if not exist "%_DIR%\echo%_SUF%" goto :eof
pushd "%_DIR%"
set "CGO_ENABLED=0"
go build -ldflags="-s -w" -o "%BIN_DIR%\%CUR_PROTO%-go-echo%_SUF%.exe" ".\echo%_SUF%" && (call :ok "%CUR_PROTO%-go-echo%_SUF% built") || (call :warn "skip go %CUR_PROTO%%_SUF% (build failed)")
popd
goto :eof

REM build_rust_server <suffix>
:build_rust_server
set "_SUF=%~1"
set "_DIR=%NET_DIR%\%CUR_PROTO%\server\rust-echo"
if not exist "%_DIR%" goto :eof
pushd "%_DIR%"
set "_OLD_RUSTFLAGS=%RUSTFLAGS%"
if defined RUSTFLAGS (set "RUSTFLAGS=%RUSTFLAGS% -C strip=symbols") else (set "RUSTFLAGS=-C strip=symbols")
cargo build --release -q --bin %CUR_PROTO%-rust-echo%_SUF% && copy /Y "target\release\%CUR_PROTO%-rust-echo%_SUF%.exe" "%BIN_DIR%\" >nul && (call :ok "%CUR_PROTO%-rust-echo%_SUF% built") || (call :warn "skip rust %CUR_PROTO%%_SUF% (build failed)")
if defined _OLD_RUSTFLAGS (set "RUSTFLAGS=%_OLD_RUSTFLAGS%") else (set "RUSTFLAGS=")
popd
goto :eof

REM Locate the xylem static lib produced by CMake (name/location varies by gen).
:find_xylem_lib
set "XYLEM_LIB="
if exist "%BUILD_DIR%\xylem.lib"         set "XYLEM_LIB=%BUILD_DIR%\xylem.lib" & goto :eof
if exist "%BUILD_DIR%\Release\xylem.lib" set "XYLEM_LIB=%BUILD_DIR%\Release\xylem.lib" & goto :eof
if exist "%BUILD_DIR%\libxylem.a"        set "XYLEM_LIB=%BUILD_DIR%\libxylem.a" & goto :eof
for /r "%BUILD_DIR%" %%F in (xylem.lib libxylem.a) do (
    if exist "%%F" set "XYLEM_LIB=%%F" & goto :eof
)
goto :eof

REM ============================================================================
REM bench
REM ============================================================================
:cmd_bench
call :ensure_bin || exit /b 1
call :ensure_winclient_helper || exit /b 1

for /f "delims=" %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss" 2^>nul') do set "TS=%%T"
set "RUN_DIR=%RESULTS_ROOT%\%TS%"
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"

call :info "results to %RUN_DIR%   (MT workers = %SERVER_NCPU%)"
call :info "protocols: %PROTO%"
if "%PIN_ENABLE%"=="true" (
    call :info "core-pinning: server mask %SRV_MASK% / %SERVER_NCPU% cores, client mask %CLI_MASK% / %CLIENT_NCPU% cores (GOMAXPROCS), of %NCPU%"
) else (
    call :info "core-pinning: off (set PIN=on to enable; needs >=4 cores)"
)
echo.

for %%P in (%PROTO:,= %) do call :bench_proto %%P

call :ok "benchmarks complete"
call :info "results written to %RUN_DIR%"
goto :eof

:ensure_bin
for %%P in (%PROTO:,= %) do (
    if not exist "%BIN_DIR%\%%P-bench.exe" (
        call :err "binaries missing in %BIN_DIR%; run: %SCRIPT_NAME% build"
        exit /b 1
    )
)
goto :eof

:ensure_winclient_helper
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
set "__RUN_NET_BAT=%SCRIPT_PATH%"
set "__WINCLIENT_PS1=%WINCLIENT_PS1%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$m='::WINCLIENT::'; Get-Content -LiteralPath $env:__RUN_NET_BAT | Where-Object { $_.StartsWith($m) } | ForEach-Object { $_.Substring($m.Length) } | Set-Content -LiteralPath $env:__WINCLIENT_PS1 -Encoding UTF8" 1>nul 2>nul
if errorlevel 1 (
    call :err "failed to prepare internal Windows bench client helper"
    exit /b 1
)
if not exist "%WINCLIENT_PS1%" (
    call :err "internal Windows bench client helper was not created"
    exit /b 1
)
goto :eof

REM bench_proto <proto>
:bench_proto
set "CUR_PROTO=%~1"
call :proto_config "%CUR_PROTO%" || exit /b 1
REM Per-protocol payload sizes override the global default.
set "PAYLOADS=%PROTO_PAYLOADS%"
call :kill_servers 1>nul 2>nul

set "_DO_ST=false"
set "_DO_MT=false"
if /I "%MODE%"=="st"   set "_DO_ST=true"
if /I "%MODE%"=="mt"   set "_DO_MT=true"
if /I "%MODE%"=="both" set "_DO_ST=true" & set "_DO_MT=true"
if "%HAS_MT%"=="false" set "_DO_MT=false"

call :info "=== protocol: %CUR_PROTO%  (port base %PORT_BASE%) ==="
call :info "servers: %SERVERS%"
call :info "conns: %PROTO_CONNS%  payload: %PAYLOADS%  duration: %DURATION%s  mode: %MODE%"
echo.

if "%_DO_ST%"=="true" (
    for %%Y in (%PAYLOADS:,= %) do (
        for %%C in (%PROTO_CONNS:,= %) do (
            call :bench_throughput ST -echo "" %%C %%Y
        )
    )
    if "%RUN_CONNRATE%"=="true" if "%HAS_CONNRATE%"=="true" (
        for %%C in (%PROTO_CONNS:,= %) do (
            call :bench_connrate ST -echo "" %%C
        )
    )
)

if "%_DO_MT%"=="true" (
    for %%Y in (%PAYLOADS:,= %) do (
        for %%C in (%PROTO_CONNS:,= %) do (
            call :bench_throughput MT -echo-mt %SERVER_NCPU% %%C %%Y
        )
    )
    if "%RUN_CONNRATE%"=="true" if "%HAS_CONNRATE%"=="true" (
        for %%C in (%PROTO_CONNS:,= %) do (
            call :bench_connrate MT -echo-mt %SERVER_NCPU% %%C
        )
    )
)
goto :eof

:kill_servers
taskkill /F /IM %CUR_PROTO%-xylem-echo.exe    1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-xylem-echo-mt.exe 1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-go-echo.exe       1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-go-echo-mt.exe    1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-rust-echo.exe     1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-rust-echo-mt.exe  1>nul 2>nul
ping -n 2 127.0.0.1 >nul
goto :eof

REM format_conns <value> -> _FMT  (1000 -> 1k)
:format_conns
set /a _v=%~1
if %_v% GEQ 1000 (set /a _k=%_v%/1000 & set "_FMT=!_k!k") else (set "_FMT=%_v%")
goto :eof

REM format_size <bytes> -> _FMT  (1024 -> 1K, 1048576 -> 1M)
:format_size
set /a _v=%~1
if %_v% GEQ 1048576 (set /a _m=%_v%/1048576 & set "_FMT=!_m!M") else if %_v% GEQ 1024 (set /a _k=%_v%/1024 & set "_FMT=!_k!K") else (set "_FMT=%_v%B")
goto :eof

REM extract_json <file> <key> -> _JVAL  (last numeric value on a line with "key")
:extract_json
set "_JVAL="
for /f "tokens=2 delims=:" %%A in ('findstr /C:"\"%~2\"" "%~1" 2^>nul') do (
    set "_raw=%%A"
)
if defined _raw (
    set "_raw=!_raw: =!"
    set "_raw=!_raw:,=!"
    set "_JVAL=!_raw!"
)
set "_raw="
goto :eof

REM Calc-MiBPerSec <msg_per_sec> <payload_bytes> -> _MIBPS
:calc_mib_per_sec
set "_MIBPS=0"
for /f "delims=" %%M in ('powershell -NoProfile -Command "[int64][math]::Floor(([int64]%~1 * [int64]%~2) / 1048576)"') do set "_MIBPS=%%M"
goto :eof

REM bench_throughput <row_label> <bin_suffix> <workers> <conns> <payload>
:bench_throughput
set "ROW=%~1"
set "BSUFFIX=%~2"
set "WORKERS=%~3"
set "CONNS_V=%~4"
set "PAYLOAD_V=%~5"

call :format_conns %CONNS_V%
set "CONNS_LBL=%_FMT%"
call :format_size %PAYLOAD_V%
set "SIZE_LBL=%_FMT%"

set "WARMUP_LABEL="
if %BENCH_WARMUP_RUNS% GTR 0 set "WARMUP_LABEL= (+%BENCH_WARMUP_RUNS% warmup)"
call :info "=== [%CUR_PROTO%] %ROW% Throughput: c%CONNS_LBL% payload=%SIZE_LBL% %DURATION%s%WARMUP_LABEL% ==="
echo   SERVER            msg/s     MB/s    p50^(us^)    p99^(us^)    max^(us^)
echo   ------------------------------------------------------------------------

set /a _offset=0
for %%N in (%SERVERS:,= %) do (
    set "name=%%N"
    set /a port=%PORT_BASE% + !_offset!
    set "bin=%BIN_DIR%\%CUR_PROTO%-!name!%BSUFFIX%.exe"
    set /a _offset+=1

    if not exist "!bin!" (
        call :warn "skip !name! (binary %CUR_PROTO%-!name!%BSUFFIX%.exe not found)"
    ) else (
        call :start_server "!bin!" !port! "%WORKERS%" 1>nul 2>nul
        ping -n 3 127.0.0.1 >nul

        set /a tp_sum=0, p50_sum=0, p99_sum=0, max_sum=0, valid_runs=0

        set /a total_runs=%BENCH_WARMUP_RUNS% + 1
        for /l %%R in (1,1,!total_runs!) do (
            set /a measured_run=%%R - %BENCH_WARMUP_RUNS%
            if !measured_run! LEQ 0 (set "run_name=warmup%%R") else (set "run_name=r!measured_run!")
            set "out=%RUN_DIR%\%CUR_PROTO%-throughput-%ROW%-c!CONNS_LBL!-!SIZE_LBL!-!name!-!run_name!.json"
            set "start_marker=%RUN_DIR%\.window-start-!name!-!run_name!"
            set "end_marker=%RUN_DIR%\.window-end-!name!-!run_name!"
            if exist "!start_marker!" del /q "!start_marker!" >nul 2>nul
            if exist "!end_marker!" del /q "!end_marker!" >nul 2>nul
            set "_climask="
            set "_gmp=0"
            if "%PIN_ENABLE%"=="true" set "_climask=!CLI_MASK!" & set "_gmp=!CLIENT_NCPU!"
            set "_srvn=%SERVER_NCPU%"
            if "%PIN_ENABLE%"=="true" if /I "%ROW%"=="ST" set "_srvn=1"
            set "srv_cpu_line="
            for /f "delims=" %%L in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%WINCLIENT_PS1%" -Exe "%BIN_DIR%\%CUR_PROTO%-bench.exe" -OutFile "!out!" -ArgString "throughput -n !CONNS_V! -d %DURATION% -s !PAYLOAD_V! -p !port!" -CliMaskHex "!_climask!" -Gomaxprocs !_gmp! -ServerNcpu !_srvn! -StartMarker "!start_marker!" -EndMarker "!end_marker!" 2^>nul') do set "srv_cpu_line=%%L"
            if exist "!start_marker!" del /q "!start_marker!" >nul 2>nul
            if exist "!end_marker!" del /q "!end_marker!" >nul 2>nul

            if !measured_run! LEQ 0 (
                set "_sz="
            ) else (
                for %%F in ("!out!") do set "_sz=%%~zF"
            )
            if defined _sz if !_sz! GTR 0 (
                call :extract_json "!out!" throughput_msg_per_sec
                set "tp=!_JVAL!"
                call :extract_json "!out!" latency_p50_us
                set "p50=!_JVAL!"
                call :extract_json "!out!" latency_p99_us
                set "p99=!_JVAL!"
                call :extract_json "!out!" latency_max_us
                set "lat_max=!_JVAL!"
                for /f "delims=." %%X in ("!tp!") do set "tp=%%X"
                for /f "delims=." %%X in ("!p50!") do set "p50=%%X"
                for /f "delims=." %%X in ("!p99!") do set "p99=%%X"
                for /f "delims=." %%X in ("!lat_max!") do set "lat_max=%%X"
                if defined tp if !tp! GTR 0 (
                    set /a tp_sum+=tp, p50_sum+=p50, p99_sum+=p99, max_sum+=lat_max, valid_runs+=1
                )
            )
            set "_sz="
            if %%R LSS !total_runs! ping -n 2 127.0.0.1 >nul
        )

        call :proc_peak_rss "%CUR_PROTO%-!name!%BSUFFIX%.exe" 1>nul 2>nul
        set "srv_peak=!_PEAK_RSS!"
        call :stop_server 1>nul 2>nul
        ping -n 2 127.0.0.1 >nul

        if !valid_runs! GTR 0 (
            set /a tp_avg=tp_sum/valid_runs, p50_avg=p50_sum/valid_runs, p99_avg=p99_sum/valid_runs, max_avg=max_sum/valid_runs
            call :calc_mib_per_sec !tp_avg! !PAYLOAD_V!
            set "mbps=!_MIBPS!"
            echo   !name!    !tp_avg!    !mbps!    !p50_avg!    !p99_avg!    !max_avg!
            if defined srv_peak (echo              srv: peak_rss=!srv_peak!MB) else (echo              srv: peak_rss=n/a)
            if defined srv_cpu_line if not "!srv_cpu_line!"=="" echo              cpu: !srv_cpu_line!
        ) else (
            call :warn "!name!: no valid output"
        )
    )
)
echo.
goto :eof

REM bench_connrate <row_label> <bin_suffix> <workers> <concurrency>
:bench_connrate
set "ROW=%~1"
set "BSUFFIX=%~2"
set "WORKERS=%~3"
set "CONC_V=%~4"

call :format_conns %CONC_V%
set "CONC_LBL=%_FMT%"

call :info "=== [%CUR_PROTO%] %ROW% ConnRate: concurrency=%CONC_LBL% %DURATION%s ==="
echo   SERVER           conn/s      fails
echo   ------           ------      -----

set /a _offset=0
for %%N in (%SERVERS:,= %) do (
    set "name=%%N"
    set /a port=%PORT_BASE% + !_offset!
    set "bin=%BIN_DIR%\%CUR_PROTO%-!name!%BSUFFIX%.exe"
    set /a _offset+=1

    if not exist "!bin!" (
        call :warn "skip !name! (binary %CUR_PROTO%-!name!%BSUFFIX%.exe not found)"
    ) else (
        call :start_server "!bin!" !port! "%WORKERS%" 1>nul 2>nul
        ping -n 3 127.0.0.1 >nul

        set "out=%RUN_DIR%\%CUR_PROTO%-connrate-%ROW%-!CONC_LBL!-!name!.json"
        set "_climask="
        set "_gmp=0"
        if "%PIN_ENABLE%"=="true" set "_climask=!CLI_MASK!" & set "_gmp=!CLIENT_NCPU!"
        powershell -NoProfile -ExecutionPolicy Bypass -File "%WINCLIENT_PS1%" -Exe "%BIN_DIR%\%CUR_PROTO%-bench.exe" -OutFile "!out!" -ArgString "connrate -c !CONC_V! -d %DURATION% -p !port!" -CliMaskHex "!_climask!" -Gomaxprocs !_gmp! -ServerNcpu 0 >nul 2>&1

        call :stop_server 1>nul 2>nul
        ping -n 2 127.0.0.1 >nul

        for %%F in ("!out!") do set "_sz=%%~zF"
        if defined _sz if !_sz! GTR 0 (
            call :extract_json "!out!" connects_per_sec
            set "cps=!_JVAL!"
            call :extract_json "!out!" failed_connects
            set "fails=!_JVAL!"
            if not defined cps set "cps=?"
            if not defined fails set "fails=0"
            echo   !name!           !cps!      !fails!
        ) else (
            call :warn "!name!: no output"
        )
        set "_sz="
    )
)
echo.
goto :eof

REM proc_peak_rss <image.exe> -> _PEAK_RSS (peak working set in MB, or empty)
:proc_peak_rss
set "_PEAK_RSS="
for /f "usebackq delims=" %%M in (`powershell -NoProfile -Command "$p=Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension('%~1')) -ErrorAction SilentlyContinue; if ($p) { [int]((($p | Measure-Object PeakWorkingSet64 -Maximum).Maximum)/1MB) }" 2^>nul`) do set "_PEAK_RSS=%%M"
goto :eof

REM start_server <bin> <port> <workers>  -- launches server in background
:start_server
set "_bin=%~1"
set "_port=%~2"
set "_workers=%~3"
set "_aff="
if "%PIN_ENABLE%"=="true" (
    if "%_workers%"=="" (set "_aff=/affinity %SRV_ST_MASK%") else (set "_aff=/affinity %SRV_MASK%")
)
if "%_workers%"=="" (
    start "xylem-bench-srv" %_aff% /b "%_bin%" %_port% 1>nul 2>nul
) else (
    start "xylem-bench-srv" %_aff% /b "%_bin%" %_port% %_workers% 1>nul 2>nul
)
goto :eof

REM stop_server -- kill the bench server processes for the current protocol
:stop_server
taskkill /F /IM %CUR_PROTO%-xylem-echo.exe    1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-xylem-echo-mt.exe 1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-go-echo.exe       1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-go-echo-mt.exe    1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-rust-echo.exe     1>nul 2>nul
taskkill /F /IM %CUR_PROTO%-rust-echo-mt.exe  1>nul 2>nul
goto :eof

REM ============================================================================
REM usage
REM ============================================================================
:usage
echo usage: %SCRIPT_NAME% [tcp^|udp^|tls^|help]
echo.
echo Windows benchmark driver. Build auto-inits MSVC via vcvars64.bat (vswhere).
echo.
echo Arguments:
echo   tcp^|udp^|tls   build + run the full comparison matrix for that protocol
echo                  (xylem vs go vs rust; ST, and MT where the protocol has it)
echo   help           this help
echo   (none)         run every protocol: %NET_BENCH_PROTO%   [default]
echo.
echo Fixed matrix (edit NET_BENCH_* constants in this script to change it):
echo   servers:    %NET_BENCH_SERVERS%
echo   conns:      %NET_BENCH_CONNS%
echo   payloads:   %NET_BENCH_PAYLOADS%
echo   duration:   %NET_BENCH_DURATION%s
echo   mode:       %NET_BENCH_MODE%
echo   connrate:   %NET_BENCH_RUN_CONNRATE%
echo.
echo Notes:
echo   Missing toolchain pieces (cmake/ninja/go/rust) print setup guidance
echo   automatically when the run starts.
echo   TLS needs OpenSSL (vcpkg); xylem is built with -DXYLEM_ENABLE_TLS=ON.
echo   UDP has no MT row. The Windows client uses IOCP; numbers are NOT
echo   comparable to Unix runs.
echo   Throughput runs %NET_BENCH_WARMUP_RUNS% uncounted warmup pass(es).
echo.
echo Examples:
echo   %SCRIPT_NAME% tcp
echo   %SCRIPT_NAME% udp
echo   %SCRIPT_NAME% tls
echo   %SCRIPT_NAME%            all protocols
goto :eof

::WINCLIENT::# Internal Windows helper extracted by run-net.bat.
::WINCLIENT::param(
::WINCLIENT::    [string]$Exe,
::WINCLIENT::    [string]$OutFile,
::WINCLIENT::    [string]$ArgString,
::WINCLIENT::    [string]$CliMaskHex = "",
::WINCLIENT::    [int]$Gomaxprocs = 0,
::WINCLIENT::    [int]$ServerNcpu = 0,
::WINCLIENT::    [string]$StartMarker = "",
::WINCLIENT::    [string]$EndMarker = ""
::WINCLIENT::)
::WINCLIENT::
::WINCLIENT::if ($Gomaxprocs -gt 0) { $env:GOMAXPROCS = "$Gomaxprocs" }
::WINCLIENT::$ClientArgs = $ArgString.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
::WINCLIENT::
::WINCLIENT::function Sample-Cores($n) {
::WINCLIENT::    $rows = Get-CimInstance Win32_PerfRawData_PerfOS_Processor -ErrorAction SilentlyContinue |
::WINCLIENT::            Where-Object { $_.Name -match '^\d+$' -and [int]$_.Name -lt $n }
::WINCLIENT::    $m = @{}
::WINCLIENT::    foreach ($r in $rows) { $m[[int]$r.Name] = @($r.PercentProcessorTime, $r.Timestamp_Sys100NS) }
::WINCLIENT::    return $m
::WINCLIENT::}
::WINCLIENT::
::WINCLIENT::$fallbackBefore = $null
::WINCLIENT::$before = $null
::WINCLIENT::$after = $null
::WINCLIENT::if ($ServerNcpu -gt 0) { $fallbackBefore = Sample-Cores $ServerNcpu }
::WINCLIENT::if ($StartMarker -ne "") {
::WINCLIENT::    Remove-Item -LiteralPath $StartMarker -ErrorAction SilentlyContinue
::WINCLIENT::    $env:BENCH_WINDOW_START_FILE = $StartMarker
::WINCLIENT::}
::WINCLIENT::if ($EndMarker -ne "") {
::WINCLIENT::    Remove-Item -LiteralPath $EndMarker -ErrorAction SilentlyContinue
::WINCLIENT::    $env:BENCH_WINDOW_END_FILE = $EndMarker
::WINCLIENT::}
::WINCLIENT::
::WINCLIENT::$ErrFile = [System.IO.Path]::GetTempFileName()
::WINCLIENT::$p = Start-Process -FilePath $Exe -ArgumentList $ClientArgs `
::WINCLIENT::        -RedirectStandardOutput $OutFile -RedirectStandardError $ErrFile `
::WINCLIENT::        -PassThru -WindowStyle Hidden
::WINCLIENT::if ($CliMaskHex -ne "") {
::WINCLIENT::    try { $p.ProcessorAffinity = [IntPtr]([Convert]::ToInt64($CliMaskHex, 16)) } catch {}
::WINCLIENT::}
::WINCLIENT::if ($ServerNcpu -gt 0 -and $StartMarker -ne "" -and $EndMarker -ne "") {
::WINCLIENT::    while (-not $p.HasExited -and -not (Test-Path -LiteralPath $StartMarker)) {
::WINCLIENT::        Start-Sleep -Milliseconds 20
::WINCLIENT::    }
::WINCLIENT::    if (Test-Path -LiteralPath $StartMarker) {
::WINCLIENT::        $before = Sample-Cores $ServerNcpu
::WINCLIENT::        while (-not $p.HasExited -and -not (Test-Path -LiteralPath $EndMarker)) {
::WINCLIENT::            Start-Sleep -Milliseconds 20
::WINCLIENT::        }
::WINCLIENT::        if (Test-Path -LiteralPath $EndMarker) {
::WINCLIENT::            $after = Sample-Cores $ServerNcpu
::WINCLIENT::        }
::WINCLIENT::    }
::WINCLIENT::}
::WINCLIENT::$p.WaitForExit()
::WINCLIENT::if ($StartMarker -ne "") {
::WINCLIENT::    Remove-Item Env:BENCH_WINDOW_START_FILE -ErrorAction SilentlyContinue
::WINCLIENT::    Remove-Item -LiteralPath $StartMarker -ErrorAction SilentlyContinue
::WINCLIENT::}
::WINCLIENT::if ($EndMarker -ne "") {
::WINCLIENT::    Remove-Item Env:BENCH_WINDOW_END_FILE -ErrorAction SilentlyContinue
::WINCLIENT::    Remove-Item -LiteralPath $EndMarker -ErrorAction SilentlyContinue
::WINCLIENT::}
::WINCLIENT::Remove-Item -LiteralPath $ErrFile -ErrorAction SilentlyContinue
::WINCLIENT::
::WINCLIENT::if ($ServerNcpu -gt 0 -and (-not $before -or -not $after)) {
::WINCLIENT::    $before = $fallbackBefore
::WINCLIENT::    $after = Sample-Cores $ServerNcpu
::WINCLIENT::}
::WINCLIENT::if ($ServerNcpu -gt 0 -and $before -and $after) {
::WINCLIENT::    $parts = @()
::WINCLIENT::    for ($i = 0; $i -lt $ServerNcpu; $i++) {
::WINCLIENT::        if ($before.ContainsKey($i) -and $after.ContainsKey($i)) {
::WINCLIENT::            $dIdle = [double]($after[$i][0] - $before[$i][0])
::WINCLIENT::            $dTime = [double]($after[$i][1] - $before[$i][1])
::WINCLIENT::            $pct = 0
::WINCLIENT::            if ($dTime -gt 0) { $pct = [math]::Round((1 - $dIdle / $dTime) * 100) }
::WINCLIENT::            if ($pct -lt 0) { $pct = 0 }
::WINCLIENT::            if ($pct -gt 100) { $pct = 100 }
::WINCLIENT::            $parts += "cpu${i}:${pct}%"
::WINCLIENT::        }
::WINCLIENT::    }
::WINCLIENT::    Write-Output ($parts -join " ")
::WINCLIENT::}
