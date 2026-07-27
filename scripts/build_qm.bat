@if not defined _echo echo off

rem Builds a provided build config as the QM replay beta variant (CnCNet-QM-Spawner.dll).

rem Ensure we're in correct directory.
cd /D "%~dp0"

call run_msbuild /maxCpuCount /consoleloggerparameters:NoSummary /property:Configuration=%1 /property:SpawnerVariant=QM
