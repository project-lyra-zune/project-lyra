#pragma once

#include "nativeapp_common.h"

// Cross-process diagnostic dump: dumps the target PID's module list +
// each module's PE export table to \flash2\automation\probe-<pid>.log.
// Runs inside nativeapp.exe (separate process from compositor/gemstone),
// so PE-walker faults are contained via SEH.
BOOL ProbePidToFile(DWORD target_pid, DWORD* out_err);
