@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

if exist out rmdir /s /q out

cmake -B out -G Ninja -DXYLEM_ENABLE_TLS=ON -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_COVERAGE=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if %ERRORLEVEL% neq 0 (
    echo [ERROR] cmake configure failed
    exit /b 1
)

cmake --build out -j 8
if %ERRORLEVEL% neq 0 (
    echo [ERROR] build failed
    exit /b 1
)

copy /Y out\compile_commands.json compile_commands.json 2>nul

ctest --test-dir out --output-on-failure
set TEST_EXIT=%ERRORLEVEL%

cmake --build out --target coverage
set COV_EXIT=%ERRORLEVEL%

if exist out\coverage\index.html start out\coverage\index.html

if %TEST_EXIT% neq 0 (
    echo [WARN] some tests failed
)
if %COV_EXIT% neq 0 (
    echo [WARN] coverage generation failed
)

exit /b %TEST_EXIT%
