#!/usr/bin/env bash
set -e

rm -rf out

cmake -B out \
  -DXYLEM_ENABLE_TLS=ON \
  -DXYLEM_ENABLE_TESTING=ON \
  -DXYLEM_ENABLE_COVERAGE=OFF \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build out -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cp -f out/compile_commands.json compile_commands.json 2>/dev/null || true

set +e
ctest --test-dir out --output-on-failure
TEST_EXIT=$?

cmake --build out --target coverage
COV_EXIT=$?
set -e

if [ -f out/coverage/html/index.html ]; then
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open out/coverage/html/index.html
  elif command -v open >/dev/null 2>&1; then
    open out/coverage/html/index.html
  fi
fi

[ $TEST_EXIT -ne 0 ] && echo "[WARN] some tests failed"
[ $COV_EXIT -ne 0 ] && echo "[WARN] coverage generation failed"

exit $TEST_EXIT
