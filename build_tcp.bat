@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d C:\Users\wujin\Desktop\workspace\Xylem

cmake --build out --config Debug --target test-tcp -j 8
if %errorlevel% neq 0 exit /b %errorlevel%

ctest --test-dir out -C Debug -R tcp --output-on-failure
