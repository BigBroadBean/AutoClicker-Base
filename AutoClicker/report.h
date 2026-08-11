#pragma once

// ============================================================
//  HWID usage reporting (startup, fire-and-forget)
// ============================================================
// At startup the program sends a one-time GET request to the hardcoded
// report server (domain + port, no config file, see servercfg.h):
//
//     http://counter.bigbroadbean.top:3000/report?hwid=HW-...
//
// The HWID is a stable per-machine identifier (MachineGuid-derived), used to
// count how many unique machines run the program.
//
// The request runs on a detached background thread with a 5s total timeout,
// so a dead / unreachable server never blocks or delays the UI. Results are
// appended to %APPDATA%\AutoClicker\report.log for diagnostics.

void StartHwidReporter();
