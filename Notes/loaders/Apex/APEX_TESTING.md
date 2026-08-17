# Building and Running the Apex Tests on Windows

This guide describes how to build and run the Apex XT Auto tests with Qt 6.9.3
and MinGW 13.1 on Windows.

## What the Apex tests cover

The Apex tests are registered in OSCAR's existing Qt Test executable through
`CONFIG+=test`. They cover:

- APF session-record decoding and table parsing.
- APE circular-buffer arithmetic and session decoding.
- APE session-table parsing.
- Mapping decoded pressure and leak data to OSCAR channels.

The tests use the same `UNITTEST_MODE` configuration as OSCAR's other unit
tests. No Apex-specific build configuration is required.

## Prepare a clean test-build directory

Open PowerShell in the root of your OSCAR source checkout and run:

```powershell
New-Item -ItemType Directory -Force .\build\test1
Set-Location .\build\test1
```

Add the installed Qt and MinGW tools to the current terminal's `PATH`:

```powershell
$env:Path = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.9.3\mingw_64\bin;$env:Path"
```

## Generate the normal test build

Generate the Makefile using OSCAR's standard test configuration:

```powershell
qmake ..\..\oscar\oscar.pro -spec win32-g++ "CONFIG+=test"
```

Then compile:

```powershell
mingw32-make -j $env:NUMBER_OF_PROCESSORS SHELL=cmd.exe
```

## Existing MinGW test-build problem

A clean test build may stop with an error similar to:

```text
<sstream>: error: '__xfer_bufptrs' redeclared with different access
```

This is not caused by the Apex parser or Apex tests. The same failure was
reproduced from the local `master` branch at commit
`0c6fa5b54511b73e18a974150ee2472bae460784`, before the Apex changes were
present.

The existing test configuration defines `UNITTEST_MODE`. Some legacy SleepLib
headers respond by defining:

```cpp
#define private public
#define protected public
```

Those macros can remain active when a later Qt header includes the MinGW C++
standard library. They alter access keywords inside `<sstream>`, causing the
compiler error. Known non-Apex examples include `profileimporter.cpp` and
`sefam_loader.cpp`.

The Apex implementation avoids introducing another instance of this problem by
including its Qt headers before `apex_loader.h`. Both `apex_loader.o` and
`apextests.o`, including `testLeakChannelMapping`, compile without an Apex
`<sstream>` workaround.

## Temporary command-line bypass for the full test suite

Until the common OSCAR test infrastructure is corrected, regenerate the
Makefile with `<sstream>` loaded before the legacy access macros:

```powershell
qmake ..\..\oscar\oscar.pro -spec win32-g++ "CONFIG+=test" "QMAKE_CXXFLAGS+=-include sstream"
```

Compile the complete test executable:

```powershell
mingw32-make -j $env:NUMBER_OF_PROCESSORS SHELL=cmd.exe
```

This option affects only the generated test build. It does not add an
`<sstream>` dependency to the Apex loader and does not affect regular OSCAR
builds.

## Run the tests

After a successful build, run the complete registered test suite:

```powershell
.\test.exe
```

Check the process exit code:

```powershell
$LASTEXITCODE
```

An exit code of `0` means all executed tests passed.

Qt Test can list the registered test functions with:

```powershell
.\test.exe -functions
```

Because OSCAR runs multiple registered Qt Test classes through one executable,
the complete `test.exe` run is the most reliable validation.

## Rebuild after source changes

If the Makefile is already configured with the temporary bypass, rebuild and
run with:

```powershell
Set-Location .\build\test1
$env:Path = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.9.3\mingw_64\bin;$env:Path"
mingw32-make -j $env:NUMBER_OF_PROCESSORS SHELL=cmd.exe
.\test.exe
```

Example of output:
```
********* Start testing of ApexTests *********
Config: Using QtTest library 6.9.3, Qt 6.9.3 (x86_64-little_endian-llp64 shared (dynamic) release build; by GCC 13.1.0), windows 11
PASS   : ApexTests::initTestCase()
PASS   : ApexTests::testDecodeApfRecord()
PASS   : ApexTests::testApfEmptySlotStopsTable()
PASS   : ApexTests::testApfTableCapacity()
PASS   : ApexTests::testRingAdvanceWraps()
PASS   : ApexTests::testDecodeApeSessionRun_basic()
PASS   : ApexTests::testDecodeApeSessionRun_wrapsAcrossRingEnd()
PASS   : ApexTests::testDecodeApeSessionRun_optionalZeroPrefix()
PASS   : ApexTests::testDecodeApeSessionRun_outOfRangePointer()
PASS   : ApexTests::testDecodeApeSessionRun_missingMarker()
PASS   : ApexTests::testDecodeApeSessionRun_unterminatedExceedsMaxMinutes()
PASS   : ApexTests::testParseApe_matchesByExactTimestamp()
PASS   : ApexTests::testParseApe_staleEntrySkippedSilently()
PASS   : ApexTests::testParseApe_unusedTableEntriesSkipped()
PASS   : ApexTests::testLeakChannelMapping()
PASS   : ApexTests::cleanupTestCase()
Totals: 16 passed, 0 failed, 0 skipped, 0 blacklisted, 10ms
********* Finished testing of ApexTests *********
```