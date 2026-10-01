DVBStreamer5 Stage 9 V10.8.5 — TBS6909-X / STiD135 tuning hotfix

Expected base:
  main @ cc41e36 (V10.8.4)

What changes:
- frontend kernel module detection via sysfs
- reads /sys/module/<driver>/parameters/mode
- STiD135 mode 0: select complete RF path before DiSEqC
- STiD135 mode 1: direct-LNB sequence with safer delays
- STiD135 mode 2: explicit safe rejection until SCR/user-band parameters exist
- diseqc_source 4..7 no longer silently aliases 0..3
- NATIVE DVB TUNE debug logging
- STiD135 FE_READ_STATUS polling 200 ms

Apply on Windows PowerShell:

  cd D:\PROJECTS\DVBStreamer5
  git status
  powershell -ExecutionPolicy Bypass -File .\apply_stage9_v10_8_5.ps1 -RepoPath D:\PROJECTS\DVBStreamer5

Then inspect:

  git diff -- src/media/LinuxDvbInput.cpp

Build in WSL after the same source is available there:

  cd /home/monki/DVBStreamer5
  cmake --build build-test --parallel 2 --target DVBStreamer5

Run:

  ./build-test/DVBStreamer5 2>&1 | tee /tmp/dvb-v1085.log

Diagnostics:

  grep -E "NATIVE DVB TUNE|STiD135|lock" /tmp/dvb-v1085.log

Do not commit/push until build and TBS lock tests succeed.
