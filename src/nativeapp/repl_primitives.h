#ifndef REPL_PRIMITIVES_H
#define REPL_PRIMITIVES_H

// REPL primitives dispatched by nativeapp's wire handler (opcode 40).
//
// One call dispatches one (sub_opcode, args) pair to one of the 15
// handlers (READ_PROC_MEM, KREAD_BYTES, KCALL, etc. See
// repl_primitives.cpp for the SUB_* enum + per-handler wire shape).
//
// Args:
//   arg / arg_len: u32 LE sub_opcode followed by sub-handler args
//   out_v / out_max: response buffer (caller-owned; must hold at least
//                    the 4-byte status header)
//   out_used: caller-allocated; set to bytes actually written into out_v
//
// Returns: 0 on dispatch success (out_v[0..3] carries handler status);
// -1 on malformed call (arg_len < 4 or out_max < 8).
int repl_dispatch(const void* arg, int arg_len,
                  void* out_v, int out_max, int* out_used);

#endif
