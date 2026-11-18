@echo off
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  echo Run this script with CMake and an x64 MSVC toolchain available.
  exit /b 1
)
if not defined XENGINE_TEST_MYSQL_HOST (
  echo Set XENGINE_TEST_MYSQL_HOST, XENGINE_TEST_MYSQL_PORT, XENGINE_TEST_MYSQL_USER and XENGINE_TEST_MYSQL_PASSWORD for an isolated test server.
  exit /b 1
)
set "taskPeerRoot=%~dp0..\..\..\..\XTrader\XTrader"
set "PATH=%taskPeerRoot%\exec\Debug;%taskPeerRoot%\libs\debug;%PATH%"
set "taskTestDir=%TEMP%\xengine-mysql-tests-%RANDOM%-%RANDOM%"
cmake -S "%~dp0" -B "%taskTestDir%" -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
cmake --build "%taskTestDir%" --target strategy_execution_tests
if errorlevel 1 exit /b 1
ctest --test-dir "%taskTestDir%" -R strategy_execution_tests --output-on-failure
exit /b %errorlevel%
