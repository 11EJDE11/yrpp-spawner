@if not defined _echo echo off

rem Builds YRpp-Spawner Release-CnCNetYR as the QM replay beta variant (CnCNet-QM-Spawner.dll).

rem Ensure we're in correct directory.
cd /D "%~dp0"

call build_qm Release-CnCNetYR
