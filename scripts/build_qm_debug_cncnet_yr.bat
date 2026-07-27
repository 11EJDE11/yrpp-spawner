@if not defined _echo echo off

rem Builds YRpp-Spawner Debug-CnCNetYR as the QM replay beta variant (CnCNet-QM-Spawner.dll).

rem Ensure we're in correct directory.
cd /D "%~dp0"

call build_qm Debug-CnCNetYR
