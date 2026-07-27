@if not defined _echo echo off

rem Builds YRpp-Spawner Release-CnCNetYR-Hardened as the QM replay beta variant (CnCNet-QM-Spawner.dll).
rem This is the config shipped to Quick Match players - it includes the anti-cheat code.

rem Ensure we're in correct directory.
cd /D "%~dp0"

call build_qm Release-CnCNetYR-Hardened
