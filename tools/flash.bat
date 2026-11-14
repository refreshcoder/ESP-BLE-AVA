@echo off
REM Flash the 4 image files produced by a build (or by the CI artifact) with esptool.
REM
REM   tools\flash.bat                  ^<- build\  + auto-detect port, esp32c6
REM   tools\flash.bat COM7             ^<- explicit port
REM   set CHIP=esp32c3 & tools\flash.bat
REM
REM Requires: pip install esptool

setlocal
if "%~1"=="" (set PORTARG=) else (set PORTARG=-p %~1)
if "%CHIP%"=="" (set CHIP=esp32c6)
if "%BINDIR%"=="" (set BINDIR=build)

if not exist "%BINDIR%\beacon_gw.bin" (
  echo missing %BINDIR%\beacon_gw.bin - run "idf.py build" first, or set BINDIR
  exit /b 1
)

echo chip=%CHIP%  bindir=%BINDIR%  port=%1
esptool --chip %CHIP% %PORTARG% -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB ^
  0x0 "%BINDIR%\bootloader\bootloader.bin" ^
  0x8000 "%BINDIR%\partition_table\partition-table.bin" ^
  0xf000 "%BINDIR%\ota_data_initial.bin" ^
  0x20000 "%BINDIR%\beacon_gw.bin"

echo.
echo SoftAP BeaconGW-XXXX / password beaconXXXX / http://192.168.4.1/
endlocal
