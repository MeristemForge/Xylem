@echo off
setlocal EnableDelayedExpansion

REM ============================================================================
REM Xylem benchmark suite (Windows, native cmd.exe)
REM ----------------------------------------------------------------------------
REM Run this from a "Developer Command Prompt for VS 2022" (or any cmd.exe that
REM has run vcvarsall.bat x64) so cl.exe, cmake and ninja are on PATH. See
REM docs\build.md ("Windows: Developer Command Prompt") for setup.
REM
REM   install  - guidance for installing deps via winget (Go, Rust, CMake, Ninja)
REM   build    - build xylem + echo servers (per protocol) + bench client
REM   bench    - run comparison benchmarks and write results\<ts>\
REM   all      - build + bench                                       [default]
REM
REM Protocols (--proto, comma-separated): tcp, udp, tls
REM   tcp : stream echo,   ports from 9000, ST + MT, throughput + connrate
REM   udp : datagram echo, ports from 9001, ST only, throughput
REM   tls : TLS-over-TCP,  ports from 9443, ST + MT, throughput + connrate
REM         (xylem built with -DXYLEM_ENABLE_TLS=ON; needs OpenSSL via vcpkg)
REM
REM Compared servers (default): xylem, go, rust. libuv/boost are omitted by
REM default (require vcpkg); missing binaries are skipped automatically. UDP
REM has no MT row.
REM
REM NOTE: the Windows bench client uses IOCP (*-bench-win.c). Windows lacks
REM SO_REUSEPORT / /proc; per-CPU usage sampling is unavailable and numbers are
REM NOT comparable to the Linux or macOS suites. Server peak working-set is
REM reported via PowerShell Get-Process.
REM ============================================================================

REM ---- paths -----------------------------------------------------------------
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "BENCH_DIR=%%~fI"
for %%I in ("%BENCH_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "BIN_DIR=%BENCH_DIR%\bin"
set "BUILD_DIR=%BENCH_DIR%\build"
set "RESULTS_ROOT=%BENCH_DIR%\results"

REM ---- defaults (env vars seed them; CLI overrides) --------------------------
if not defined PROTO    set "PROTO=tcp"
if not defined SERVERS  set "SERVERS=xylem,go,rust"
if not defined CONNS    set "CONNS=1000,10000"
if not defined PAYLOADS set "PAYLOADS=64,4096,65536"
if not defined DURATION set "DURATION=10"
if not defined MODE     set "MODE=both"
if not defined REPEAT   set "REPEAT=1"
set "RUN_CONNRATE=true"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=4"
set "NCPU=%NUMBER_OF_PROCESSORS%"

REM ---- dispatch --------------------------------------------------------------
set "CMD=%~1"
if "%CMD%"=="" set "CMD=all"
if not "%CMD%"=="" shift

if /I "%CMD%"=="install" goto :cmd_install
if /I "%CMD%"=="build"   goto :do_build
if /I "%CMD%"=="bench"   goto :do_bench
if /I "%CMD%"=="all"     goto :do_all
if /I "%CMD%"=="-h"      goto :usage
if /I "%CMD%"=="--help"  goto :usage
if /I "%CMD%"=="help"    goto :usage
call :err "unknown command: %CMD%"
goto :usage

:do_build
call :parse_bench_opts %*
call :cmd_build
goto :eof

:do_bench
call :parse_bench_opts %*
call :cmd_bench
goto :eof

:do_all
call :parse_bench_opts %*
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
REM per-protocol config: sets PORT_BASE / HAS_MT / HAS_CONNRATE / IS_TLS
REM ============================================================================
:proto_config
set "_p=%~1"
if /I "%_p%"=="tcp" ( set "PORT_BASE=9000" & set "HAS_MT=true"  & set "HAS_CONNRATE=true"  & set "IS_TLS=false" & goto :eof )
if /I "%_p%"=="udp" ( set "PORT_BASE=9001" & set "HAS_MT=false" & set "HAS_CONNRATE=false" & set "IS_TLS=false" & goto :eof )
if /I "%_p%"=="tls" ( set "PORT_BASE=9443" & set "HAS_MT=true"  & set "HAS_CONNRATE=true"  & set "IS_TLS=true"  & goto :eof )
call :err "unknown protocol: %_p% (must be tcp|udp|tls)"
exit /b 1

REM ============================================================================
REM install (guidance only)
REM ============================================================================
:cmd_install
call :info "Windows dependency setup"
echo   Required toolchain (run from an elevated PowerShell / cmd):
echo.
echo     winget install Kitware.CMake
echo     winget install Ninja-build.Ninja
echo     winget install GoLang.Go
echo     winget install Rustlang.Rustup
echo.
echo   Visual Studio 2022 (MSVC C/C++ toolset) must be installed, and this
echo   script must run from a "Developer Command Prompt for VS 2022".
echo.
echo   For TLS (and libuv/boost servers) via vcpkg:
echo     git clone https://github.com/microsoft/vcpkg
echo     .\vcpkg\bootstrap-vcpkg.bat
echo     .\vcpkg\vcpkg install openssl libuv boost-asio
echo.
where cl >nul 2>&1
if errorlevel 1 (
    call :warn "cl.exe not found on PATH -- launch from a VS Developer environment."
) else (
    call :ok "cl.exe detected"
)
goto :eof

REM ============================================================================
REM build
REM ============================================================================
:cmd_build
where cl >nul 2>&1
if errorlevel 1 (
    call :err "cl.exe not found. Run from a VS Developer environment (see: %~nx0 install)."
    exit /b 1
)
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

REM xylem TLS flag = ON if tls is among the requested protocols
set "TLS_FLAG=OFF"
for %%P in (%PROTO:,= %) do if /I "%%P"=="tls" set "TLS_FLAG=ON"

set "USE_NINJA=false"
where ninja >nul 2>&1 && set "USE_NINJA=true"

call :info "building xylem static library (XYLEM_ENABLE_TLS=%TLS_FLAG%)..."
if "%USE_NINJA%"=="true" (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DXYLEM_ENABLE_TLS=%TLS_FLAG% >nul 2>&1
    cmake --build "%BUILD_DIR%" --target xylem -j %NCPU% >nul 2>&1
) else (
    cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -DXYLEM_ENABLE_TLS=%TLS_FLAG% >nul 2>&1
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

set "CL_FLAGS=/nologo /O2 /DNDEBUG /MT /W3"
set "SYS_LIBS=ws2_32.lib mswsock.lib psapi.lib"

for %%P in (%PROTO:,= %) do call :build_proto %%P
echo.
dir /a-d "%BIN_DIR%"
goto :eof

REM build_proto <proto>
:build_proto
set "CUR_PROTO=%~1"
call :proto_config "%CUR_PROTO%" || exit /b 1

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

REM ---- bench client (native Win32 / IOCP) -----------------------------------
call :info "building %CUR_PROTO%-bench client..."
cl %CL_FLAGS% "%BENCH_DIR%\%CUR_PROTO%\client\%CUR_PROTO%-bench-win.c" %SYS_LIBS% %EXTRA_LIBS% /Fe:"%BIN_DIR%\%CUR_PROTO%-bench.exe" >nul 2>&1
if errorlevel 1 (
    call :err "failed to build %CUR_PROTO%-bench client"
    exit /b 1
)
call :ok "%CUR_PROTO%-bench built"
goto :eof

REM build_xylem_server <suffix>
:build_xylem_server
set "_SUF=%~1"
set "_SRC=%BENCH_DIR%\%CUR_PROTO%\server\xylem-echo%_SUF%.c"
set "_OUT=%BIN_DIR%\%CUR_PROTO%-xylem-echo%_SUF%.exe"
if not exist "%_SRC%" goto :eof
call :info "building %CUR_PROTO%-xylem-echo%_SUF%..."
cl %CL_FLAGS% /I"%PROJECT_ROOT%\include" "%_SRC%" "%XYLEM_LIB%" %SYS_LIBS% %EXTRA_LIBS% /Fe:"%_OUT%" >nul 2>&1
if errorlevel 1 (
    call :warn "skip xylem %CUR_PROTO%%_SUF% (build failed)"
) else (
    call :ok "%CUR_PROTO%-xylem-echo%_SUF% built"
)
goto :eof

REM build_go_server <suffix>
:build_go_server
set "_SUF=%~1"
set "_DIR=%BENCH_DIR%\%CUR_PROTO%\server\go-echo%_SUF%"
if not exist "%_DIR%" goto :eof
pushd "%_DIR%"
set "CGO_ENABLED=0"
go build -ldflags="-s -w" -o "%BIN_DIR%\%CUR_PROTO%-go-echo%_SUF%.exe" . && (call :ok "%CUR_PROTO%-go-echo%_SUF% built") || (call :warn "skip go %CUR_PROTO%%_SUF% (build failed)")
popd
goto :eof

REM build_rust_server <suffix>
:build_rust_server
set "_SUF=%~1"
set "_DIR=%BENCH_DIR%\%CUR_PROTO%\server\rust-echo"
if not exist "%_DIR%" goto :eof
pushd "%_DIR%"
cargo build --release -q --bin %CUR_PROTO%-rust-echo%_SUF% && copy /Y "target\release\%CUR_PROTO%-rust-echo%_SUF%.exe" "%BIN_DIR%\" >nul && (call :ok "%CUR_PROTO%-rust-echo%_SUF% built") || (call :warn "skip rust %CUR_PROTO%%_SUF% (build failed)")
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

for /f "tokens=2 delims==" %%T in ('wmic os get localdatetime /value 2^>nul ^| find "="') do set "_DT=%%T"
set "TS=%_DT:~0,8%-%_DT:~8,6%"
set "RUN_DIR=%RESULTS_ROOT%\%TS%"
if not exist "%RUN_DIR%" mkdir "%RUN_DIR%"

call :info "results -> %RUN_DIR%   (MT workers = %NCPU%)"
call :info "protocols: %PROTO%"
echo.

for %%P in (%PROTO:,= %) do call :bench_proto %%P

call :ok "benchmarks complete"
call :info "results written to %RUN_DIR%"
goto :eof

:ensure_bin
for %%P in (%PROTO:,= %) do (
    if not exist "%BIN_DIR%\%%P-bench.exe" (
        call :err "binaries missing in %BIN_DIR%; run: %~nx0 build --proto %PROTO%"
        exit /b 1
    )
)
goto :eof

REM bench_proto <proto>
:bench_proto
set "CUR_PROTO=%~1"
call :proto_config "%CUR_PROTO%" || exit /b 1
call :kill_servers

set "_DO_ST=false"
set "_DO_MT=false"
if /I "%MODE%"=="st"   set "_DO_ST=true"
if /I "%MODE%"=="mt"   set "_DO_MT=true"
if /I "%MODE%"=="both" set "_DO_ST=true" & set "_DO_MT=true"
if "%HAS_MT%"=="false" set "_DO_MT=false"

call :info "=== protocol: %CUR_PROTO%  (port base %PORT_BASE%) ==="
call :info "servers: %SERVERS%"
call :info "conns: %CONNS%  payload: %PAYLOADS%  duration: %DURATION%s  mode: %MODE%"
echo.

if "%_DO_ST%"=="true" (
    for %%Y in (%PAYLOADS:,= %) do (
        for %%C in (%CONNS:,= %) do (
            call :bench_throughput ST -echo "" %%C %%Y
        )
    )
    if "%RUN_CONNRATE%"=="true" if "%HAS_CONNRATE%"=="true" (
        for %%C in (%CONNS:,= %) do (
            call :bench_connrate ST -echo "" %%C
        )
    )
)

if "%_DO_MT%"=="true" (
    for %%Y in (%PAYLOADS:,= %) do (
        for %%C in (%CONNS:,= %) do (
            call :bench_throughput MT -echo-mt %NCPU% %%C %%Y
        )
    )
    if "%RUN_CONNRATE%"=="true" if "%HAS_CONNRATE%"=="true" (
        for %%C in (%CONNS:,= %) do (
            call :bench_connrate MT -echo-mt %NCPU% %%C
        )
    )
)
goto :eof

:kill_servers
taskkill /F /IM %CUR_PROTO%-xylem-echo.exe    >nul 2>&1
taskkill /F /IM %CUR_PROTO%-xylem-echo-mt.exe >nul 2>&1
taskkill /F /IM %CUR_PROTO%-go-echo.exe       >nul 2>&1
taskkill /F /IM %CUR_PROTO%-go-echo-mt.exe    >nul 2>&1
taskkill /F /IM %CUR_PROTO%-rust-echo.exe     >nul 2>&1
taskkill /F /IM %CUR_PROTO%-rust-echo-mt.exe  >nul 2>&1
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

call :info "=== [%CUR_PROTO%] %ROW% Throughput: c%CONNS_LBL% payload=%SIZE_LBL% %DURATION%s x%REPEAT% ==="
echo   SERVER       msg/s(avg)     MB/s    p50(us)    p99(us)    max(us)  runs
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
        call :start_server "!bin!" !port! "%WORKERS%"
        ping -n 3 127.0.0.1 >nul

        set /a tp_sum=0, p50_sum=0, p99_sum=0, max_sum=0, valid_runs=0
        set "tp_vals="

        for /l %%R in (1,1,%REPEAT%) do (
            set "out=%RUN_DIR%\%CUR_PROTO%-throughput-%ROW%-c!CONNS_LBL!-!SIZE_LBL!-!name!-r%%R.json"
            "%BIN_DIR%\%CUR_PROTO%-bench.exe" throughput -n !CONNS_V! -d %DURATION% -s !PAYLOAD_V! -p !port! > "!out!" 2>nul

            for %%F in ("!out!") do set "_sz=%%~zF"
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
                    if defined tp_vals (set "tp_vals=!tp_vals!,!tp!") else (set "tp_vals=!tp!")
                )
            )
            set "_sz="
            if %%R LSS %REPEAT% ping -n 2 127.0.0.1 >nul
        )

        call :proc_peak_rss "%CUR_PROTO%-!name!%BSUFFIX%.exe"
        set "srv_peak=!_PEAK_RSS!"
        call :stop_server
        ping -n 2 127.0.0.1 >nul

        if !valid_runs! GTR 0 (
            set /a tp_avg=tp_sum/valid_runs, p50_avg=p50_sum/valid_runs, p99_avg=p99_sum/valid_runs, max_avg=max_sum/valid_runs
            set /a mbps=tp_avg*PAYLOAD_V/1048576
            if %REPEAT% GTR 1 (
                echo   !name!    !tp_avg!    !mbps!    !p50_avg!    !p99_avg!    !max_avg!  [!tp_vals!]
            ) else (
                echo   !name!    !tp_avg!    !mbps!    !p50_avg!    !p99_avg!    !max_avg!
            )
            if defined srv_peak (echo              srv: peak_rss=!srv_peak!MB) else (echo              srv: peak_rss=n/a)
        ) else (
            call :warn "!name!: no valid output from %REPEAT% runs"
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
        call :start_server "!bin!" !port! "%WORKERS%"
        ping -n 3 127.0.0.1 >nul

        set "out=%RUN_DIR%\%CUR_PROTO%-connrate-%ROW%-!CONC_LBL!-!name!.json"
        "%BIN_DIR%\%CUR_PROTO%-bench.exe" connrate -c !CONC_V! -d %DURATION% -p !port! > "!out!" 2>nul

        call :stop_server
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
if "%_workers%"=="" (
    start "xylem-bench-srv" /b "%_bin%" %_port% >nul 2>&1
) else (
    start "xylem-bench-srv" /b "%_bin%" %_port% %_workers% >nul 2>&1
)
goto :eof

REM stop_server -- kill the bench server processes for the current protocol
:stop_server
taskkill /F /IM %CUR_PROTO%-xylem-echo.exe    >nul 2>&1
taskkill /F /IM %CUR_PROTO%-xylem-echo-mt.exe >nul 2>&1
taskkill /F /IM %CUR_PROTO%-go-echo.exe       >nul 2>&1
taskkill /F /IM %CUR_PROTO%-go-echo-mt.exe    >nul 2>&1
taskkill /F /IM %CUR_PROTO%-rust-echo.exe     >nul 2>&1
taskkill /F /IM %CUR_PROTO%-rust-echo-mt.exe  >nul 2>&1
goto :eof

REM ============================================================================
REM option parsing  (env vars seed defaults; CLI overrides)
REM ============================================================================
:parse_bench_opts
if "%~1"=="" goto :eof
set "_opt=%~1"
if /I "%_opt%"=="--proto"   (set "PROTO=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-P"        (set "PROTO=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--servers" (set "SERVERS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-s"        (set "SERVERS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--conns"   (set "CONNS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-c"        (set "CONNS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--payload" (set "PAYLOADS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-S"        (set "PAYLOADS=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--duration" (set "DURATION=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-d"        (set "DURATION=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--mode"    (call :set_mode "%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-m"        (call :set_mode "%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--repeat"  (set "REPEAT=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="-r"        (set "REPEAT=%~2" & shift & shift & goto :parse_bench_opts)
if /I "%_opt%"=="--no-connrate" (set "RUN_CONNRATE=false" & shift & goto :parse_bench_opts)
call :err "unknown option: %_opt%"
exit /b 1

:set_mode
set "MODE=%~1"
if /I "%MODE%"=="st"   goto :eof
if /I "%MODE%"=="mt"   goto :eof
if /I "%MODE%"=="both" goto :eof
call :err "invalid mode: %MODE% (must be st|mt|both)"
exit /b 1

REM ============================================================================
REM usage
REM ============================================================================
:usage
echo usage: %~nx0 [install^|build^|bench^|all] [options...]
echo.
echo Windows benchmark driver. Run from a "Developer Command Prompt for VS 2022".
echo.
echo Commands:
echo   install   print winget/vcpkg setup guidance, verify cl.exe
echo   build     xylem static lib + per-protocol servers + bench clients
echo   bench     run comparison benchmarks, write benchmark\results\^<ts^>\
echo   all       build + bench   (default)
echo.
echo Options (pass after the command; env vars seed defaults):
echo   --proto, -P    tcp,udp,tls        protocols (default: tcp); udp is ST only
echo   --servers, -s  xylem,go,rust      servers to compare (comma-separated)
echo   --conns, -c    1000,10000         connection counts (comma-separated)
echo   --payload, -S  64,4096,65536      payload sizes in bytes (comma-separated)
echo   --duration, -d 10                 test duration in seconds
echo   --mode, -m     st^|mt^|both         single-thread / multi-thread / both
echo   --repeat, -r   3                  repeat each test N times (avg results)
echo   --no-connrate                     skip connection-rate tests
echo.
echo Notes:
echo   TLS needs OpenSSL (vcpkg); xylem is built with -DXYLEM_ENABLE_TLS=ON when
echo   tls is among the protocols. UDP has no MT row. The Windows client uses
echo   IOCP; numbers are NOT comparable to Unix runs.
echo.
echo Examples:
echo   %~nx0 build --proto tcp,udp,tls
echo   %~nx0 bench --proto tls --servers xylem,go,rust --conns 1000 --duration 5
echo   %~nx0 bench -P udp -s xylem,rust -c 1000,5000 -d 15 --mode st
goto :eof
