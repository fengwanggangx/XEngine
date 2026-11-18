@echo off
setlocal
where cl >nul 2>nul
if errorlevel 1 (
  echo Run this script in an x64 Visual Studio Developer Command Prompt.
  exit /b 1
)
set "taskProjectRoot=%~dp0..\.."
set "taskTestDir=%TEMP%\xengine-execution-%RANDOM%-%RANDOM%"
mkdir "%taskTestDir%"
if errorlevel 1 exit /b 1
pushd "%taskTestDir%"
cl /nologo /EHsc /std:c++20 /utf-8 /W4 /WX /MD /FeStrategyExecutionTests.exe "%~dp0StrategyExecutionTests.cpp" "%taskProjectRoot%\strategy\CStrategyExecution.cpp" "%taskProjectRoot%\trade\StrategyAccountView.cpp" "%taskProjectRoot%\trade\TradingTypes.cpp" "%taskProjectRoot%\request\MarketTypes.cpp" /link winsqlite3.lib
set "taskCompileResult=%errorlevel%"
if not "%taskCompileResult%"=="0" (
  popd
  exit /b %taskCompileResult%
)
StrategyExecutionTests.exe "%taskTestDir%\execution.sqlite3"
set "taskTestResult=%errorlevel%"
popd
exit /b %taskTestResult%
