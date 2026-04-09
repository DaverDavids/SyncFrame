@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" (
    echo Usage: drag coredump.bin onto this file.
    pause
    exit /b 1
)

pushd "%~dp0"

set "BIN_FILE=%~1"
set "APP_ELF=%cd%\build\SF-ESP32S3.ino.elf"
set "IDF_PATH=C:\esp\v6.0\esp-idf"
set "TOOLS_ROOT=%USERPROFILE%\.espressif\tools"
set "ESP_COREDUMP=%IDF_PATH%\components\espcoredump\espcoredump.py"
set "ROM_ELF=C:\esp\rom-elfs\esp32s3_rev0_rom.elf"
set "GDB_EXE=C:\Users\rawdr\.espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin\xtensa-esp32s3-elf-gdb.exe"

if not exist "%BIN_FILE%" (
    echo coredump file not found:
    echo "%BIN_FILE%"
    goto :fail
)

if not exist "%APP_ELF%" (
    echo ELF not found:
    echo "%APP_ELF%"
    goto :fail
)

if not exist "%IDF_PATH%\export.bat" (
    echo export.bat not found:
    echo "%IDF_PATH%\export.bat"
    goto :fail
)

if not exist "%ESP_COREDUMP%" (
    echo espcoredump.py not found:
    echo "%ESP_COREDUMP%"
    goto :fail
)

REM set "GDB_EXE="

REM for /d %%D in ("%TOOLS_ROOT%\xtensa-esp32s3-elf\*") do (
REM     for /f "delims=" %%F in ('dir /b /s "%%~fD\xtensa-esp32s3-elf-gdb.exe" 2^>nul') do (
REM        set "GDB_EXE=C:\Users\rawdr\.espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\xtensa-esp32s3-elf-gdb.exe"
REM    )
REM )

if not defined GDB_EXE (
    echo Could not find xtensa-esp-elf-gdb.exe under:
    echo "%TOOLS_ROOT%"
    goto :fail
)

echo Input: "%BIN_FILE%"
echo ELF:   "%APP_ELF%"
echo GDB:   "%GDB_EXE%"
echo.

call "%IDF_PATH%\export.bat" >nul

python "%ESP_COREDUMP%" --chip esp32s3 info_corefile ^
  -t raw -c "%BIN_FILE%" ^
  --gdb "%GDB_EXE%" ^
  --rom-elf "%ROM_ELF%" ^
  "%APP_ELF%" > coredump_report.txt 2>&1
goto :done

:fail
echo.
echo Failed.
:done
popd
pause