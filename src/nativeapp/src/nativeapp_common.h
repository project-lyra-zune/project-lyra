#pragma once

#include <windows.h>
#include <zdk.h>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <assert.h>
#include <compclient.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <zdkinput.h>
#include <zdkgl.h>
#include <zdksystem.h>
#include <zdknet.h>
#include <zam.h>
#include <znet.h>
#include <wininet.h>
#include <winsock2.h>
#include <Iphlpapi.h>
#include "protocol/pb_encode.h"
#include "protocol/pb_decode.h"
#include "protocol/msg.pb.h"
#include "repl_primitives.h"
#include "kerncore.h"

#define INBUFSZ 1024
#define OBUFSZ 0x10000
// Keep <= RespRdfile.data max_size (msg.options); encoded msg must fit OBUFSZ.
#define RDFILESZ 0xC000

typedef unsigned int u32;
typedef unsigned short u16;

typedef BOOL (*VCE)(DWORD, void*, HANDLE, DWORD, DWORD, DWORD);
typedef void* (*CSM)(DWORD, DWORD);

// Shared scratch buffer for swprintf into TCP responses. Single-threaded
// use only (no synchronization).
extern wchar_t foo[256];

// RPC server shutdown flag. connection() opcode 11 sets it to abort the
// daemon's accept loop.
extern bool dead;

// Reused protobuf response builder. Reset per request via _init_zero.
extern zunecom_CommandResp resp;
