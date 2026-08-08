@echo off
setlocal EnableDelayedExpansion
set DIR=%~dp0
:::set DIR=\oscar\oscar-code\oscar\
cd %DIR%

:: Check git in path and git directory can be found
where /Q git.exe
if errorlevel 1 goto GitFail

git rev-parse --git-dir >nul 2>&1
if errorlevel 1 goto GitFail

  for /f %%i in ('git rev-parse --abbrev-ref HEAD') do set GIT_BRANCH=%%i
  if "%GIT_BRANCH%"=="HEAD" set GIT_BRANCH=
  :: GIT_BRANCH is embedded as semver build metadata (version.cpp), which only allows
  :: [0-9A-Za-z-] (dot-separated) - see update_gitinfo.sh for the full explanation.
  :: Branch names routinely contain "/" (e.g. "fix/foo"), so replace it with "-".
  if defined GIT_BRANCH set "GIT_BRANCH=%GIT_BRANCH:/=-%"
  for /f %%i in ('git rev-parse --short HEAD') do set GIT_REVISION=%%i

  git diff-index --quiet HEAD --
  if %errorlevel%==0 goto GitTag
    set GIT_REVISION=%GIT_REVISION%-plus
    set GIT_TAG=
    goto GitDone
  :GitTag
    for /f %%i in ('git describe --exact-match --tags') do set GIT_TAG=%%i
    goto GitDone
    
:GitFail
:GitDone

@echo Update_gitinfo.bat: GIT_BRANCH=%GIT_BRANCH%, GIT_REVISION=%GIT_REVISION%, GIT_TAG=%GIT_TAG%

echo // This is an auto generated file > %DIR%git_info.new

if "%GIT_BRANCH%"=="" goto DoRevision
  echo #define GIT_BRANCH "%GIT_BRANCH%" >> %DIR%git_info.new
:DoRevision
if "%GIT_REVISION%"=="" goto DoTag
  echo #define GIT_REVISION "%GIT_REVISION%" >> %DIR%git_info.new
:DoTag
if "%GIT_TAG%"=="" goto DefinesDone
  echo #define GIT_TAG "%GIT_TAG%" >> %DIR%git_info.new
:DefinesDone

fc %DIR%git_info.h %DIR%git_info.new 1>nul 2>nul && del /q %DIR%git_info.new || goto NewFile
goto AllDone

:NewFile
echo Updating %DIR%git_info.h
move /y %DIR%git_info.new %DIR%git_info.h

:AllDone
endlocal
