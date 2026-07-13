#include "rpc_server.h"
#include "kernel_helpers.h"
#include "kerncore.h"
#include "bytepack.h"
#include "net_helpers.h"
#include "boot_runtime.h"
#include "op_diag.h"
#include "probes.h"

// ── background-daemon plugin registry ────────────────────────────────────
// Opcode 21 (SPAWN_PLUGIN_DAEMON) loads a plugin and runs its daemon entry
//   int RunDaemon(const void* arg, int arg_len, HANDLE stop_event)
// on a tracked thread, then returns a daemon id immediately; the REPL
// (1337) is free while the daemon runs. The entry runs until stop_event is
// signalled. Opcode 22 (STOP_PLUGIN_DAEMON) signals stop, joins the thread,
// and only THEN FreeLibrary's the module (no unload while the thread runs).
typedef int (*PfnDaemonEntry)(const void*, int, HANDLE);

#define MAX_DAEMONS 8
struct DaemonSlot {
	int     used;
	DWORD   id;
	HMODULE hmod;
	HANDLE  hthread;
	HANDLE  stop_event;
};
static struct DaemonSlot g_daemons[MAX_DAEMONS];
static DWORD g_next_daemon_id = 1;

struct DaemonStartCtx {
	PfnDaemonEntry fn;
	void*  arg;
	int    arg_len;
	HANDLE stop_event;
};

static DWORD WINAPI daemon_thread_thunk(LPVOID param) {
	struct DaemonStartCtx c = *(struct DaemonStartCtx*)param;
	free(param);
	__try {
		c.fn(c.arg, c.arg_len, c.stop_event);
	} __except (EXCEPTION_EXECUTE_HANDLER) { }
	if (c.arg) free(c.arg);
	return 0;
}

enum { OP_CONTINUE = 0, OP_BREAK = 1 };
typedef int (*RpcOpFn)(SOCKET client, unsigned char* inbuf, int res, unsigned char* out);
struct RpcOpEntry { unsigned char op; RpcOpFn fn; };

static int op_1(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 addr = read_u32(inbuf + 1);

				u32 val = kerncore_kreadu32(addr);

				
				out[0] = 1;
				write_u32(out + 1, val);
				
				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
			// openproc
	return OP_CONTINUE;
}

static int op_2(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 id = read_u32(inbuf + 1);
				u32 val = (u32)OpenProcess(PROCESS_ALL_ACCESS, false, id);
				out[0] = 2;
				write_u32(out + 1, val);

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
				//rd
	return OP_CONTINUE;
}

static int op_3(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 hdl = read_u32(inbuf + 1);
				u32 addr = read_u32(inbuf + 5);
				
				DWORD val=0;
				DWORD tmp=0;
				BOOL ret = ReadProcessMemory((HANDLE)hdl, (void*)addr, &tmp, 4, &val);
				DWORD err = GetLastError();

				out[0] = 3;
				write_u32(out + 1, tmp);
				write_u32(out + 5, val);
				out[9] = (ret) & 0xFF;
				write_u32(out + 10, err);

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}

				// proc w
	return OP_CONTINUE;
}

static int op_4(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 hdl = read_u32(inbuf + 1);
				u32 addr = read_u32(inbuf + 5);
				u32 val = read_u32(inbuf + 9);
				
				DWORD tmp=0;
				
				BOOL ret = WriteProcessMemory((HANDLE)hdl, (void*)addr, &val, 4, &tmp);
				DWORD err = GetLastError();

				out[0] = 4;
				write_u32(out + 1, tmp);
				out[5] = (ret) & 0xFF;
				write_u32(out + 6, err);

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_5(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 id = read_u32(inbuf + 1);
				BOOL val = DebugActiveProcess(id);

				out[0] = 5;
				write_u32(out + 1, val);

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_6(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				DEBUG_EVENT evt;
				BOOL val = WaitForDebugEvent(&evt, 0);

				u32 tmp = evt.dwDebugEventCode;

				out[0] = 6;
				write_u32(out + 1, val);
				write_u32(out + 5, tmp);

				u32 tmp1=0;

				if(val) {

					tmp1 = evt.dwProcessId;
					write_u32(out + 9, tmp1);
					
					tmp1 = evt.dwThreadId;
					write_u32(out + 13, tmp1);


					switch (tmp) {
						case EXCEPTION_DEBUG_EVENT:							
							tmp1 = evt.u.Exception.ExceptionRecord.ExceptionCode;
							write_u32(out + 17, tmp1);

							break;
						//default:
						//	ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, DBG_CONTINUE);
					}
				}



				

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_7(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 dwProcessId = read_u32(inbuf + 1);
				u32 dwThreadId = read_u32(inbuf + 5);
				
				ContinueDebugEvent(dwProcessId, dwThreadId, DBG_CONTINUE);

				out[0] = 7;

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_8(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 dwThreadId = read_u32(inbuf + 1);
			//	u32 dwThreadId = read_u32(inbuf + 5);
							
				CONTEXT ctx = {0};
				ctx.ContextFlags = CONTEXT_FULL;
				HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, false, dwThreadId);
				GetThreadContext(h, &ctx);

				int i = 0;
				out[i++] = 8;
				u32 val = ctx.R0;  write_u32(out + i, val);  i += 4;
				val = ctx.R1;      write_u32(out + i, val);  i += 4;
				val = ctx.R2;      write_u32(out + i, val);  i += 4;
				val = ctx.R3;      write_u32(out + i, val);  i += 4;
				val = ctx.Pc;      write_u32(out + i, val);  i += 4;
				val = ctx.Lr;      write_u32(out + i, val);  i += 4;
				val = ctx.Sp;      write_u32(out + i, val);  i += 4;

				// r4-r12 appended after the original 7 regs (offsets of the
				// first 7 are preserved). CONTEXT_FULL already populated these.
				DWORD extra[9];
				int e;
				extra[0] = ctx.R4;  extra[1] = ctx.R5;  extra[2] = ctx.R6;
				extra[3] = ctx.R7;  extra[4] = ctx.R8;  extra[5] = ctx.R9;
				extra[6] = ctx.R10; extra[7] = ctx.R11; extra[8] = ctx.R12;
				for (e = 0; e < 9; e++) {
					write_u32(out + i, extra[e]);
					i += 4;
				}

				CloseHandle(h);
				

				if (send(client,(char*)out,80,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}


				// proc r bulk
	return OP_CONTINUE;
}

static int op_26(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				/* OP_DEBUG_DETACH - opcode 26.
				   Symmetric with opcode 5 (DebugActiveProcess). Lets a host
				   tool clean up its debug attach without rebooting the
				   device. Without this, an interrupted bp-stackwalk session
				   leaves the daemon-side debug state attached and the next
				   attach attempt fails.
				   Wire format:
				     Request:  [0]=26, [1-4]=process_id (u32 LE)
				     Response: [0]=26, [1-4]=BOOL return (u32 LE) */
				u32 id = read_u32(inbuf + 1);
				BOOL val = DebugActiveProcessStop(id);

				out[0] = 26;
				write_u32(out + 1, val);

				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_9(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format:
				//   Request:  [0]=9, [1-4]=handle, [5-8]=addr, [9-12]=length (u32 LE)
				//   Response: 32-byte header + N bytes data, where N == bytes_read.
				//     header [0]=9, [1-4]=bytes_read, [5-8]=GetLastError,
				//     [9]=ret flag, [10-31]=reserved
				u32 hdl = read_u32(inbuf + 1);
				u32 addr = read_u32(inbuf + 5);
				u32 len = read_u32(inbuf + 9);

				const u32 BULK_RPM_MAX = 16384;  // bounded to keep response within OBUFSZ
				if (len > BULK_RPM_MAX) len = BULK_RPM_MAX;

				DWORD bytes_read = 0;
				BOOL ret = ReadProcessMemory((HANDLE)hdl, (void*)addr, &out[32], len, &bytes_read);
				DWORD err = GetLastError();

				out[0] = 9;
				write_u32(out + 1, bytes_read);
				write_u32(out + 5, err);
				out[9] = ret ? 1 : 0;
				// out[10..31] are zeroed by the memset at the top of each iteration

				if (safe_send(client, out, 32 + bytes_read)) {
					closesocket(client);
					return OP_BREAK;
				}

				// quit
	return OP_CONTINUE;
}

static int op_10(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				out[0] = 10;
				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
					return OP_BREAK;
				}
				closesocket(client);
				// kill
	return OP_CONTINUE;
}

static int op_11(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				out[0] = 10;  // kill shares the quit reply code (10)
				if (send(client,(char*)out,32,0) == SOCKET_ERROR){
					closesocket(client);
				}
				closesocket(client);
				dead = true;
				return OP_BREAK;

				// write-file (chunked)
	return OP_CONTINUE;
}

static int op_12(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format:
				//   Request 32-byte header:
				//     [0]=12, [1-4]=path_length, [5-8]=file_offset, [9-12]=data_length
				//   Then path_length bytes (UTF-8 path) - separate recv
				//   Then data_length bytes (file content) - separate recv
				//   Response 32 bytes:
				//     [0]=12, [1-4]=bytes_written, [5-8]=GetLastError,
				//     [9]=ret flag, [10-31]=reserved
				//
				// file_offset == 0 → CREATE_ALWAYS (truncate). Nonzero offset →
				// OPEN_ALWAYS + seek; supports chunked writes for large files.
				u32 path_length = read_u32(inbuf + 1);
				u32 file_offset = read_u32(inbuf + 5);
				u32 data_length = read_u32(inbuf + 9);

				const u32 WRFILE_PATH_MAX = 256;
				const u32 WRFILE_DATA_MAX = 16384;

				if (path_length == 0 || path_length > WRFILE_PATH_MAX || data_length > WRFILE_DATA_MAX) {
					memset(out, 0, 32);
					out[0] = 12;
					// err = ERROR_INVALID_PARAMETER (87)
					out[5] = 87;
					out[9] = 0;
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client);
					return OP_BREAK;
				}

				struct recv_stream rs = { client, inbuf, 32, res };

				char pathbuf[WRFILE_PATH_MAX + 1];
				if (stream_consume(&rs, pathbuf, path_length) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}
				pathbuf[path_length] = 0;

				wchar_t wpath[WRFILE_PATH_MAX + 1];
				std::swprintf(wpath, L"%S", pathbuf);

				if (stream_consume(&rs, out, data_length) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}

				DWORD disposition = (file_offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS;
				HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
				DWORD err = GetLastError();
				DWORD bytes_written = 0;
				BOOL ret = FALSE;

				if (h != INVALID_HANDLE_VALUE) {
					if (file_offset != 0) {
						SetFilePointer(h, (LONG)file_offset, NULL, FILE_BEGIN);
					}
					if (data_length > 0) {
						ret = WriteFile(h, out, data_length, &bytes_written, NULL);
						err = GetLastError();
					} else {
						ret = TRUE;
					}
					CloseHandle(h);
				}

				memset(out, 0, 32);
				out[0] = 12;
				write_u32(out + 1, bytes_written);
				write_u32(out + 5, err);
				out[9] = ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}

				// mkdir
	return OP_CONTINUE;
}

static int op_13(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format:
				//   Request 32-byte header:
				//     [0]=13, [1-4]=path_length (u32 LE, 1..256), [5-31] reserved
				//   Then path_length bytes (UTF-8 path)
				//   Response 32 bytes:
				//     [0]=13, [1-4]=GetLastError (u32 LE),
				//     [5]=ret flag (1 = CreateDirectoryW returned TRUE),
				//     [6-31] reserved
				//
				// ERROR_ALREADY_EXISTS (183) is left for the caller to interpret;
				// idempotent treatment is a host-side policy choice.
				u32 path_length = read_u32(inbuf + 1);

				if (path_length == 0 || path_length > 256) {
					memset(out, 0, 32);
					out[0] = 13;
					out[1] = 87; // ERROR_INVALID_PARAMETER
					out[5] = 0;
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client);
					return OP_BREAK;
				}

				// Bare recv() ignored bytes the dispatch loop pre-buffered into
				// inbuf[32..res]; over PPP/USB-tunnel that bundles header+path in
				// one packet, the bytes are stuck in the buffer and recv() blocks
				// forever waiting on a socket that has nothing left to send.
				// stream_consume drains the residual first, exactly like opcode 12.
				struct recv_stream rs = { client, inbuf, 32, res };

				char pathbuf[257];
				if (stream_consume(&rs, pathbuf, path_length) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}
				pathbuf[path_length] = 0;

				wchar_t wpath[257];
				std::swprintf(wpath, L"%S", pathbuf);

				BOOL ret = CreateDirectoryW(wpath, NULL);
				DWORD err = GetLastError();

				memset(out, 0, 32);
				out[0] = 13;
				write_u32(out + 1, err);
				out[5] = ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}

				// rm-file
	return OP_CONTINUE;
}

static int op_14(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format mirrors opcode 13:
				//   Request 32-byte header: [0]=14, [1-4]=path_length, [5-31] reserved
				//   Then path_length bytes (UTF-8 path).
				//   Response 32 bytes: [0]=14, [1-4]=GetLastError, [5]=ret flag,
				//     [6-31] reserved.
				u32 rm_path_length = read_u32(inbuf + 1);

				if (rm_path_length == 0 || rm_path_length > 256) {
					memset(out, 0, 32);
					out[0] = 14;
					out[1] = 87; // ERROR_INVALID_PARAMETER
					out[5] = 0;
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client);
					return OP_BREAK;
				}

				// Same residual-buffer bug as opcode 13. See that handler.
				struct recv_stream rs = { client, inbuf, 32, res };

				char rm_pathbuf[257];
				if (stream_consume(&rs, rm_pathbuf, rm_path_length) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}
				rm_pathbuf[rm_path_length] = 0;

				wchar_t rm_wpath[257];
				std::swprintf(rm_wpath, L"%S", rm_pathbuf);

				BOOL rm_ret = DeleteFileW(rm_wpath);
				DWORD rm_err = GetLastError();

				memset(out, 0, 32);
				out[0] = 14;
				write_u32(out + 1, rm_err);
				out[5] = rm_ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}
	return OP_CONTINUE;
}

static int op_15(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

u32 idx = read_u32(inbuf + 1);
u32 idx2 = read_u32(inbuf + 5);
u32 val = read_u32(inbuf + 9);
	
				//char* out = (char*)calloc(cccc, 4);
				#if 1
					kwr(0x80060da0, 0x80069de0);

					HMODULE mh = GetModuleHandleW(L"coredll.dll");
					//NKCreateStaticMapping
					CSM csm = (CSM) GetProcAddress(mh, L"GetFSHeapInfo");

					SetLastError(0);
					//void* ahb_arb = csm(0x6000c000, 0x1000);
					//void* boorom = csm(0xFFF00000>>8, 0x10000);
					//void* boorom1 = csm(0xFFF02000>>8, 0x10000);

					DWORD sec_boot = (DWORD)csm(0x60000000>>8, 0x1000);
					kwr(sec_boot+0xc200, 1);


					DWORD clk = (DWORD)csm(0x60006000>>8, 0x1000);

					// enable iram{a,b,c,d}
					DWORD clk_rst_controller_clk_out_enb_u_0 = kerncore_kreadu32(clk + 0x18);
					clk_rst_controller_clk_out_enb_u_0 |= (1<<20);
					clk_rst_controller_clk_out_enb_u_0 |= (1<<21);
					clk_rst_controller_clk_out_enb_u_0 |= (1<<22);
					clk_rst_controller_clk_out_enb_u_0 |= (1<<23);
					kwr(clk+0x18, clk_rst_controller_clk_out_enb_u_0);



					//void* f = csm(0x40000000>>8, 0x10000);

					void* f = (void*)sec_boot;//csm(idx>>8, val);

					kwr(0x80060da0, 0x80015020);

					for(u32 j=idx2;j<val;j++) {
						char out[1];
						out[0] = (char)kerncore_kreadb((DWORD)f + j);
						if (send(client,(char*)out,1,0) == SOCKET_ERROR){
							closesocket(client);
							break;
						}
						//Sleep(100);
					}
				#endif
	return OP_CONTINUE;
}

static int op_16(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

	zunecom_CommandReq msg = zunecom_CommandReq_init_zero;
	pb_istream_t pbstream = pb_istream_from_buffer(&inbuf[1], res-1);
	if(!pb_decode(&pbstream, zunecom_CommandReq_fields, &msg)) {
		memset(out, 0, 128);
		memset(foo, 0, 128);
		std::swprintf(foo, L" pb err: %S r: %d", PB_GET_ERROR(&pbstream), res-1);
		memcpy(out, foo, 128);
		out[0] = 0xCD;

		if (safe_send(client,(unsigned char*)out, 128)){
			closesocket(client);
			return OP_BREAK;
		}
		return OP_CONTINUE;
	}
	pb_ostream_t ostream = pb_ostream_from_buffer(out, OBUFSZ);
	

	WIN32_FIND_DATA ffd;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    BOOL r=1;
	char tmpbuf[200];

	switch(msg.cmd) {
		case zunecom_CommandReq_CommandType_CMD_LSDIR:
		{
		// Streaming lsdir: one CommandResp per directory entry (cmd=
		// RSP_LSDIR_ENT, payload=lsdir), then a final cmd=RSP_LSDIR_EOF
		// with payload=eof to terminate. Mirrors RSP_RDFILE_DATA /
		// RSP_RDFILE_EOF. Replaces the fixed-array RespLsdir that
		// truncated when the encoded array exceeded OBUFSZ.
		memset(foo, 0, 256);
		std::swprintf(foo, L"%S", msg.payload.lsdir.path);
		DWORD lsdir_probe_err = ERROR_SUCCESS;
		hFind = find_first_with_retry(foo, &ffd, &lsdir_probe_err, 4000);

		bool lsdir_empty = false;
		if (INVALID_HANDLE_VALUE == hFind) {
			if (lsdir_probe_err == ERROR_NO_MORE_FILES) {
				// empty dir gives an empty listing (EOF below), not an error
				lsdir_empty = true;
			} else {
				refresh_file_runtime_state();
				if (send_resp_err(
					client,
					lsdir_probe_err,
					"lsdir path=%S err=%lu flash2=%d/%lu",
					foo,
					lsdir_probe_err,
					g_file_runtime.flash2_ready ? 1 : 0,
					g_file_runtime.flash2_error)) {
					closesocket(client);
				}
				break;
			}
		}

		// path length cap: matches the regenerated msg.pb.h field width
		// (RespLsdir.path is char[300]; leave 1 byte for terminator).
		#define LSDIR_PATH_CAP 298

		bool lsdir_done = lsdir_empty;
		bool lsdir_first = true;
		while (!lsdir_done) {
			if (!lsdir_first) {
				r = FindNextFile(hFind, &ffd);
				if (r == 0) { lsdir_done = true; break; }
			}
			lsdir_first = false;

			resp.cmd = zunecom_CommandResp_ResType_RSP_LSDIR_ENT;
			resp.which_payload = zunecom_CommandResp_lsdir_tag;
			wcstombs(tmpbuf, ffd.cFileName, LSDIR_PATH_CAP);
			tmpbuf[LSDIR_PATH_CAP - 1] = 0;
			strncpy(resp.payload.lsdir.path, tmpbuf, LSDIR_PATH_CAP);
			resp.payload.lsdir.path[LSDIR_PATH_CAP - 1] = 0;
			resp.payload.lsdir.is_dir =
				(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? true : false;

			ostream = pb_ostream_from_buffer(out, OBUFSZ);
			if (!pb_encode(&ostream, zunecom_CommandResp_fields, &resp)) {
				memset(out, 0, 128);
				std::swprintf(foo, L"pb err: '%S' path=%S",
				              PB_GET_ERROR(&pbstream), resp.payload.lsdir.path);
				memcpy(out, foo, 256);
				out[0] = 0xCF;
				if (safe_send(client, (unsigned char*)out, 256)) {
					closesocket(client);
				}
				FindClose(hFind);
				goto lsdir_done_label;
			}
			if (safe_send(client, (unsigned char*)out, ostream.bytes_written)) {
				closesocket(client);
				FindClose(hFind);
				goto lsdir_done_label;
			}
		}

		// Terminator: cmd=RSP_LSDIR_EOF, payload=eof (empty).
		resp.cmd = zunecom_CommandResp_ResType_RSP_LSDIR_EOF;
		resp.which_payload = zunecom_CommandResp_eof_tag;
		ostream = pb_ostream_from_buffer(out, OBUFSZ);
		if (!pb_encode(&ostream, zunecom_CommandResp_fields, &resp)) {
			memset(out, 0, 128);
			std::swprintf(foo, L"pb err (lsdir eof): %S", PB_GET_ERROR(&pbstream));
			memcpy(out, foo, 256);
			out[0] = 0xCF;
			if (safe_send(client, (unsigned char*)out, 256)) {
				closesocket(client);
			}
		} else if (safe_send(client, (unsigned char*)out, ostream.bytes_written)) {
			closesocket(client);
		}
		if (!lsdir_empty) {
			FindClose(hFind);
		}
		lsdir_done_label:;
		}
	break;

		case zunecom_CommandReq_CommandType_CMD_RDFILE:
			{
				std::swprintf(foo, L"%S", msg.payload.rdfile.path);
				DWORD open_err = ERROR_SUCCESS;
				HANDLE f = open_file_with_retry(foo, &open_err, 4000);
				if (f == INVALID_HANDLE_VALUE) {
					refresh_file_runtime_state();
					if (send_resp_err(
						client,
						open_err,
						"rdfile open path=%S err=%lu flash2=%d/%lu",
						foo,
						open_err,
						g_file_runtime.flash2_ready ? 1 : 0,
						g_file_runtime.flash2_error)) {
						closesocket(client);
					}
					break;
				}

				DWORD start = msg.payload.rdfile.offset;
				DWORD remaining = msg.payload.rdfile.length;
				bool bounded = (remaining != 0);
				if (start != 0) {
					SetFilePointer(f, (LONG)start, NULL, FILE_BEGIN);
				}

				DWORD hFz = 0;
				DWORD lowfz = GetFileSize(f, &hFz);
				if (lowfz == INVALID_FILE_SIZE) {
					DWORD size_err = GetLastError();
					if (size_err != ERROR_SUCCESS) {
						CloseHandle(f);
						if (send_resp_err(client, size_err, "rdfile size path=%S err=%lu", foo, size_err)) {
							closesocket(client);
						}
						break;
					}
				}

			  
			  DWORD cnt = 0;
			  while(true) {
				cnt = 0;

				DWORD want = RDFILESZ - 50;
				if (bounded) {
					if (remaining == 0) {
						break;
					}
					if (remaining < want) {
						want = remaining;
					}
				}

				resp.cmd = zunecom_CommandResp_ResType_RSP_RDFILE_DATA;
				resp.which_payload = zunecom_CommandResp_rdfile_tag;

				r = ReadFile(f, &resp.payload.rdfile.data.bytes[0], want, &cnt, NULL);
				if (r == FALSE) {
					DWORD read_err = GetLastError();
					CloseHandle(f);
					if (send_resp_err(client, read_err, "rdfile read path=%S err=%lu", foo, read_err)) {
						closesocket(client);
					}
					goto rdfile_done;
				}
				resp.payload.rdfile.data.size = cnt;
				resp.payload.rdfile.fullsz = lowfz;
				if(cnt == 0) {
					break;
				}

				ostream = pb_ostream_from_buffer(out, OBUFSZ);

				if(!pb_encode(&ostream, zunecom_CommandResp_fields, &resp)) {
					memset(out, 0, 128);
					std::swprintf(foo, L"pb err: %S", PB_GET_ERROR(&pbstream));
					memcpy(out, foo, 128);
					out[0] = 0xDF;

					if (safe_send(client,(unsigned char*)out, 128)){
						closesocket(client);
						break;
					}
				}
				if (safe_send(client,(unsigned char*)out, ostream.bytes_written)){
					closesocket(client);
					break;
				}

				if (bounded) {
					remaining -= cnt;
				}

			  }
			  CloseHandle(f);

				resp.cmd = zunecom_CommandResp_ResType_RSP_RDFILE_EOF;
				resp.which_payload = zunecom_CommandResp_eof_tag;
				
				ostream = pb_ostream_from_buffer(out, OBUFSZ);

				if(!pb_encode(&ostream, zunecom_CommandResp_fields, &resp)) {
					memset(out, 0, 128);
					std::swprintf(foo, L"pb err: %S", PB_GET_ERROR(&pbstream));
					memcpy(out, foo, 128);
					out[0] = 0xDE;

					if (safe_send(client,(unsigned char*)out, 128)){
						closesocket(client);
						break;
					}
				}

				if (safe_send(client, (unsigned char*)out, ostream.bytes_written)){
					closesocket(client);
					break;
				}
			}
rdfile_done:
			break;
	 

	default:
		out[0] = 0xFF;
		if (safe_send(client,(unsigned char*)out, 32)){
			closesocket(client);
			break;
		}
		break;
}


			// move-file (rename; sometimes succeeds where DeleteFileW
			// fails on mapped files in CE 6, since rename can unlink the
			// directory entry without invalidating the existing mapping)
	return OP_CONTINUE;
}

static int op_17(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format mirrors opcode 13/14 but with two paths:
				//   Request 32-byte header:
				//     [0]=17, [1-4]=src_path_length (u32 LE, 1..256),
				//     [5-8]=dst_path_length (u32 LE, 1..256), [9-31] reserved
				//   Then src_path_length bytes (UTF-8 src) + dst_path_length bytes (UTF-8 dst)
				//   Response 32 bytes:
				//     [0]=17, [1-4]=GetLastError (u32 LE),
				//     [5]=ret flag (1 = MoveFileW returned TRUE), [6-31] reserved
				u32 mv_src_len = read_u32(inbuf + 1);
				u32 mv_dst_len = read_u32(inbuf + 5);

				if (mv_src_len == 0 || mv_src_len > 256 || mv_dst_len == 0 || mv_dst_len > 256) {
					memset(out, 0, 32);
					out[0] = 17;
					out[1] = 87; // ERROR_INVALID_PARAMETER
					out[5] = 0;
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client);
					return OP_BREAK;
				}

				struct recv_stream rs = { client, inbuf, 32, res };

				char mv_srcbuf[257];
				if (stream_consume(&rs, mv_srcbuf, mv_src_len) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}
				mv_srcbuf[mv_src_len] = 0;

				char mv_dstbuf[257];
				if (stream_consume(&rs, mv_dstbuf, mv_dst_len) == 0xFFFFFFFFu) {
					closesocket(client); return OP_BREAK;
				}
				mv_dstbuf[mv_dst_len] = 0;

				wchar_t mv_src_w[257], mv_dst_w[257];
				std::swprintf(mv_src_w, L"%S", mv_srcbuf);
				std::swprintf(mv_dst_w, L"%S", mv_dstbuf);

				BOOL mv_ret = MoveFileW(mv_src_w, mv_dst_w);
				DWORD mv_err = GetLastError();

				memset(out, 0, 32);
				out[0] = 17;
				write_u32(out + 1, mv_err);
				out[5] = mv_ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}

			// zuxhook trigger (signals the named event zuxhook waits on
			// inside compositor.exe + gemstone.exe). Used by the plugin
			// loader: host writes \flash2\automation\plugin.dll, signals
			// via this opcode, zuxhook's PluginWaiterThread loads the
			// DLL and calls its Activate() entry point inside the host.
	return OP_CONTINUE;
}

static int op_18(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				HANDLE re_h = CreateEventW(NULL, FALSE, FALSE, L"zune-zuxhook-trigger");
				BOOL re_ret = (re_h != NULL) && SetEvent(re_h);
				DWORD re_err = GetLastError();
				if (re_h != NULL) CloseHandle(re_h);

				memset(out, 0, 32);
				out[0] = 18;
				write_u32(out + 1, re_err);
				out[5] = re_ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}

			// pid-probe (enumerates target pid's modules + exports, writes log)
	return OP_CONTINUE;
}

static int op_19(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format:
				//   Request 32-byte header: [0]=19, [1-4]=target_pid (u32 LE),
				//     [5-31] reserved
				//   Response 32 bytes: [0]=19, [1-4]=GetLastError, [5]=ret flag,
				//     [6-31] reserved
				// Side effect: writes \flash2\automation\probe-<pid>.log
				DWORD probe_pid = read_u32(inbuf + 1);
				DWORD probe_err = 0;
				BOOL probe_ret = ProbePidToFile(probe_pid, &probe_err);

				memset(out, 0, 32);
				out[0] = 19;
				write_u32(out + 1, probe_err);
				out[5] = probe_ret ? 1 : 0;

				if (safe_send(client, out, 32)) {
					closesocket(client);
					return OP_BREAK;
				}

			// plugin invoke: LoadLibraryW(dll_path) inside the daemon,
			// GetProcAddress(entry), call as PluginEntry, return rc + bytes.
			// PluginEntry signature:
			//   int (*)(const void* arg, int arg_len,
			//           void* out, int out_max, int* out_used)
			// Wrapped in __try/__except so a faulty plugin only kills the
			// invocation, not the daemon.
	return OP_CONTINUE;
}

static int op_20(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// Wire format:
				//   Request 32-byte header:
				//     [0]=20, [1-4]=path_len (u32 LE, 1..256),
				//     [5-8]=entry_len (u32 LE, 1..63),
				//     [9-12]=arg_len (u32 LE, 0..16384), [13-31] reserved
				//   Body: path UTF-8 bytes, entry name ASCII bytes, arg bytes
				//   Response 32-byte header:
				//     [0]=20, [1-4]=int_return (u32 LE),
				//     [5-8]=out_len (u32 LE), [9-12]=GetLastError, [13-31] reserved
				//   Then out_len bytes of plugin output.
				u32 pl_path_len  = read_u32(inbuf + 1);
				u32 pl_entry_len = read_u32(inbuf + 5);
				u32 pl_arg_len   = read_u32(inbuf + 9);

				if (pl_path_len == 0 || pl_path_len > 256 ||
				    pl_entry_len == 0 || pl_entry_len > 63 ||
				    pl_arg_len > 16384) {
					memset(out, 0, 32);
					out[0] = 20;
					out[9] = 87; // ERROR_INVALID_PARAMETER
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client);
					return OP_BREAK;
				}

				char pl_pathbuf[257];
				char pl_entrybuf[64];
				unsigned char* pl_argbuf = (unsigned char*)calloc(16384, 1);
				if (pl_argbuf == NULL) { closesocket(client); return OP_BREAK; }

				struct recv_stream rs = { client, inbuf, 32, res };

				if (stream_consume(&rs, pl_pathbuf, pl_path_len) == 0xFFFFFFFFu) {
					free(pl_argbuf); closesocket(client); return OP_BREAK;
				}
				pl_pathbuf[pl_path_len] = 0;

				if (stream_consume(&rs, pl_entrybuf, pl_entry_len) == 0xFFFFFFFFu) {
					free(pl_argbuf); closesocket(client); return OP_BREAK;
				}
				pl_entrybuf[pl_entry_len] = 0;

				if (stream_consume(&rs, pl_argbuf, pl_arg_len) == 0xFFFFFFFFu) {
					free(pl_argbuf); closesocket(client); return OP_BREAK;
				}

				wchar_t pl_wpath[257];
				wchar_t pl_wentry[64];
				std::swprintf(pl_wpath, L"%S", pl_pathbuf);
				std::swprintf(pl_wentry, L"%S", pl_entrybuf);

				DWORD pl_err = 0;
				int pl_rc = -1;
				int pl_out_used = 0;
				const int pl_out_cap = 8192;
				unsigned char* pl_outbuf = (unsigned char*)calloc(pl_out_cap, 1);
				if (pl_outbuf == NULL) { free(pl_argbuf); closesocket(client); return OP_BREAK; }

				HMODULE pl_h = LoadLibraryW(pl_wpath);
				if (pl_h == NULL) {
					pl_err = GetLastError();
				} else {
					typedef int (*PluginEntry)(const void*, int, void*, int, int*);
					PluginEntry pl_fn = (PluginEntry)GetProcAddress(pl_h, pl_wentry);
					if (pl_fn == NULL) {
						pl_err = GetLastError();
					} else {
						__try {
							pl_rc = pl_fn(pl_argbuf, (int)pl_arg_len, pl_outbuf, pl_out_cap, &pl_out_used);
							if (pl_out_used < 0) pl_out_used = 0;
							if (pl_out_used > pl_out_cap) pl_out_used = pl_out_cap;
						} __except (EXCEPTION_EXECUTE_HANDLER) {
							pl_err = GetExceptionCode();
							pl_out_used = 0;
						}
					}
					FreeLibrary(pl_h);
				}

				memset(out, 0, 32);
				out[0] = 20;
				write_u32(out + 1, (unsigned)pl_rc);
				write_u32(out + 5, pl_out_used);
				write_u32(out + 9, pl_err);

				if (safe_send(client, out, 32)) {
					free(pl_argbuf); free(pl_outbuf);
					closesocket(client); return OP_BREAK;
				}
				if (pl_out_used > 0) {
					if (safe_send(client, pl_outbuf, pl_out_used)) {
						free(pl_argbuf); free(pl_outbuf);
						closesocket(client); return OP_BREAK;
					}
				}
				free(pl_argbuf); free(pl_outbuf);

			// SPAWN_PLUGIN_DAEMON - LoadLibrary + run a daemon entry on a
			// tracked thread; return a daemon id immediately (REPL stays free).
			//   Request 32-byte header:
			//     [0]=21, [1-4]=path_len (1..256), [5-8]=entry_len (1..63),
			//     [9-12]=arg_len (0..256), [13-31] reserved
			//   Body: path UTF-8 + entry ASCII + arg bytes
			//   Response 32 bytes: [0]=21, [1-4]=daemon_id (0 on failure),
			//     [9-12]=GetLastError
	return OP_CONTINUE;
}

static int op_21(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 dd_path_len  = read_u32(inbuf + 1);
				u32 dd_entry_len = read_u32(inbuf + 5);
				u32 dd_arg_len   = read_u32(inbuf + 9);
				if (dd_path_len == 0 || dd_path_len > 256 ||
				    dd_entry_len == 0 || dd_entry_len > 63 || dd_arg_len > 256) {
					memset(out, 0, 32); out[0] = 21; out[9] = 87; // ERROR_INVALID_PARAMETER
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client); return OP_BREAK;
				}
				char dd_pathbuf[257]; char dd_entrybuf[64];
				void* dd_arg = (dd_arg_len > 0) ? malloc(dd_arg_len) : NULL;
				if (dd_arg_len > 0 && dd_arg == NULL) { closesocket(client); return OP_BREAK; }
				struct recv_stream dd_rs = { client, inbuf, 32, res };
				if (stream_consume(&dd_rs, dd_pathbuf, dd_path_len) == 0xFFFFFFFFu) { if (dd_arg) free(dd_arg); closesocket(client); return OP_BREAK; }
				dd_pathbuf[dd_path_len] = 0;
				if (stream_consume(&dd_rs, dd_entrybuf, dd_entry_len) == 0xFFFFFFFFu) { if (dd_arg) free(dd_arg); closesocket(client); return OP_BREAK; }
				dd_entrybuf[dd_entry_len] = 0;
				if (dd_arg_len > 0 && stream_consume(&dd_rs, dd_arg, dd_arg_len) == 0xFFFFFFFFu) { free(dd_arg); closesocket(client); return OP_BREAK; }

				wchar_t dd_wpath[257]; wchar_t dd_wentry[64];
				std::swprintf(dd_wpath, L"%S", dd_pathbuf);
				std::swprintf(dd_wentry, L"%S", dd_entrybuf);

				DWORD dd_err = 0; DWORD dd_id = 0;
				int dd_slot = -1;
				for (int i = 0; i < MAX_DAEMONS; i++) { if (!g_daemons[i].used) { dd_slot = i; break; } }
				if (dd_slot < 0) {
					dd_err = 1450; // ERROR_NO_SYSTEM_RESOURCES
					if (dd_arg) free(dd_arg);
				} else {
					HMODULE dd_h = LoadLibraryW(dd_wpath);
					if (dd_h == NULL) {
						dd_err = GetLastError();
						if (dd_arg) free(dd_arg);
					} else {
						PfnDaemonEntry dd_fn = (PfnDaemonEntry)GetProcAddress(dd_h, dd_wentry);
						HANDLE dd_ev = (dd_fn != NULL) ? CreateEventW(NULL, TRUE, FALSE, NULL) : NULL;
						struct DaemonStartCtx* dd_ctx = (dd_fn != NULL && dd_ev != NULL)
							? (struct DaemonStartCtx*)malloc(sizeof(struct DaemonStartCtx)) : NULL;
						if (dd_fn == NULL || dd_ev == NULL || dd_ctx == NULL) {
							dd_err = GetLastError(); if (dd_err == 0) dd_err = 8;
							if (dd_ev) CloseHandle(dd_ev);
							if (dd_ctx) free(dd_ctx);
							if (dd_arg) free(dd_arg);
							FreeLibrary(dd_h);
						} else {
							dd_ctx->fn = dd_fn; dd_ctx->arg = dd_arg; dd_ctx->arg_len = (int)dd_arg_len; dd_ctx->stop_event = dd_ev;
							HANDLE dd_th = CreateThread(NULL, 0, daemon_thread_thunk, dd_ctx, 0, NULL);
							if (dd_th == NULL) {
								dd_err = GetLastError();
								CloseHandle(dd_ev); free(dd_ctx); if (dd_arg) free(dd_arg); FreeLibrary(dd_h);
							} else {
								dd_id = g_next_daemon_id++;
								g_daemons[dd_slot].used = 1; g_daemons[dd_slot].id = dd_id;
								g_daemons[dd_slot].hmod = dd_h; g_daemons[dd_slot].hthread = dd_th;
								g_daemons[dd_slot].stop_event = dd_ev;
							}
						}
					}
				}
				memset(out, 0, 32); out[0] = 21;
				write_u32(out + 1, dd_id);
				write_u32(out + 9, dd_err);
				if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }

			// STOP_PLUGIN_DAEMON - signal stop, join, then FreeLibrary.
			//   Request: [0]=22, [1-4]=daemon_id
			//   Response 32 bytes: [0]=22, [1-4]=ok (1/0), [9-12]=GetLastError
	return OP_CONTINUE;
}

static int op_22(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 ds_id = read_u32(inbuf + 1);
				DWORD ds_err = 0, ds_ok = 0;
				int ds_slot = -1;
				for (int i = 0; i < MAX_DAEMONS; i++) { if (g_daemons[i].used && g_daemons[i].id == ds_id) { ds_slot = i; break; } }
				if (ds_slot < 0) {
					ds_err = 1168; // ERROR_NOT_FOUND
				} else {
					SetEvent(g_daemons[ds_slot].stop_event);
					DWORD ds_w = WaitForSingleObject(g_daemons[ds_slot].hthread, 8000);
					if (ds_w == WAIT_OBJECT_0) {
						FreeLibrary(g_daemons[ds_slot].hmod); // unload only after the thread exited
						ds_ok = 1;
					} else {
						ds_err = ds_w; // timed out: leak the module rather than unload under a running thread
					}
					CloseHandle(g_daemons[ds_slot].hthread);
					CloseHandle(g_daemons[ds_slot].stop_event);
					g_daemons[ds_slot].used = 0; g_daemons[ds_slot].id = 0;
					g_daemons[ds_slot].hmod = NULL; g_daemons[ds_slot].hthread = NULL; g_daemons[ds_slot].stop_event = NULL;
				}
				memset(out, 0, 32); out[0] = 22;
				out[1] = ds_ok & 0xFF;
				write_u32(out + 9, ds_err);
				if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }

			// REPL dispatch - direct call into repl_primitives.cpp. The
			// {sub_opcode, args} -> {status, body} handlers are linked into
			// nativeapp, so no DLL deploy and no per-call
			// LoadLibrary/FreeLibrary round-trip.
			//   Request 32-byte header:
			//     [0]=40, [1-4]=arg_len (u32 LE, 4..16384), [5-31] reserved
			//   Body: arg bytes (first 4 = sub_opcode, rest = sub_args)
			//   Response 32-byte header:
			//     [0]=40, [1-4]=rc (i32; 0 ok, -1 malformed), [5-8]=out_len
			//     (u32 LE; bytes that follow), [9-12]=GetExceptionCode (if
			//     a handler faulted; else 0), [13-31] reserved
			//   Then out_len bytes of (status u32 + response).
	return OP_CONTINUE;
}

static int op_40(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				u32 rp_arg_len = read_u32(inbuf + 1);
				if (rp_arg_len < 4 || rp_arg_len > 16384) {
					memset(out, 0, 32);
					out[0] = 40;
					out[1] = 0xFF; out[2] = 0xFF; out[3] = 0xFF; out[4] = 0xFF; // rc = -1
					out[9] = 87; // ERROR_INVALID_PARAMETER
					if (safe_send(client, out, 32)) { closesocket(client); return OP_BREAK; }
					closesocket(client); return OP_BREAK;
				}
				unsigned char* rp_argbuf = (unsigned char*)calloc(16384, 1);
				if (rp_argbuf == NULL) { closesocket(client); return OP_BREAK; }
				struct recv_stream rs = { client, inbuf, 32, res };
				if (stream_consume(&rs, rp_argbuf, rp_arg_len) == 0xFFFFFFFFu) {
					free(rp_argbuf); closesocket(client); return OP_BREAK;
				}
				const int rp_out_cap = 262144;   // 256 KB: bulk reads (raw eMMC dumps) in far fewer round-trips
				unsigned char* rp_outbuf = (unsigned char*)calloc(rp_out_cap, 1);
				if (rp_outbuf == NULL) { free(rp_argbuf); closesocket(client); return OP_BREAK; }
				DWORD rp_err = 0;
				int rp_rc = -1;
				int rp_out_used = 0;
				__try {
					rp_rc = repl_dispatch(rp_argbuf, (int)rp_arg_len,
					                      rp_outbuf, rp_out_cap, &rp_out_used);
					if (rp_out_used < 0) rp_out_used = 0;
					if (rp_out_used > rp_out_cap) rp_out_used = rp_out_cap;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					rp_err = GetExceptionCode();
					rp_out_used = 0;
				}
				memset(out, 0, 32);
				out[0] = 40;
				write_u32(out + 1, (unsigned)rp_rc);
				write_u32(out + 5, rp_out_used);
				write_u32(out + 9, rp_err);
				if (safe_send(client, out, 32)) {
					free(rp_argbuf); free(rp_outbuf);
					closesocket(client); return OP_BREAK;
				}
				if (rp_out_used > 0) {
					if (safe_send(client, rp_outbuf, rp_out_used)) {
						free(rp_argbuf); free(rp_outbuf);
						closesocket(client); return OP_BREAK;
					}
				}
				free(rp_argbuf); free(rp_outbuf);
	return OP_CONTINUE;
}

static int op_23(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// AVP-ring content tee (writes \flash2\zd-tee.pcm)
				// Request:  [1-4]=run_ms(0→8000) [5-8]=cap(0→0x300000)
				//           [9-12]=phys_base (AVP-VA 0; 0→default)
				// Response: out[0]=23, then 10 LE u32:
				//   ch(15) phys_base ctx0 blk_base blk_top? last_idx
				//   blocks bytes elapsed_ms err
				u32 run_ms = read_u32(inbuf + 1);
				u32 cap_b  = read_u32(inbuf + 5);
				u32 pbase  = read_u32(inbuf + 9);

				struct Op23Result r23 = run_op23(run_ms, cap_b, pbase);

				u32 v23[10] = {
					r23.channel, r23.ahb0, r23.segA_base, r23.segA_top,
					r23.segB_base, r23.segB_top, r23.flips,
					r23.bytes_captured, r23.elapsed_ms, r23.error_code
				};
				memset(out, 0, 48);
				out[0] = 23;
				for (int i = 0; i < 10; i++) {
					out[1+i*4+0] = (unsigned char)(v23[i]);
					out[1+i*4+1] = (unsigned char)(v23[i] >> 8);
					out[1+i*4+2] = (unsigned char)(v23[i] >> 16);
					out[1+i*4+3] = (unsigned char)(v23[i] >> 24);
				}
				if (safe_send(client, out, 48)) { closesocket(client); return OP_BREAK; }
	return OP_CONTINUE;
}

static int op_24(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// zpod USB iso audio tee
				// Request:  [1-4]=run_ms [5-8]=handle [9-12]=ep_index
				//           [13-16]=pkt
				// Response: out[0]=24, then 10 LE u32 (Op23Result order):
				//   ch handle blk_base blk_top submit_err last_ahb
				//   packets bytes elapsed_ms err
				u32 r_ms = read_u32(inbuf + 1);
				u32 hnd  = read_u32(inbuf + 5);
				u32 epi  = read_u32(inbuf + 9);
				u32 pkt  = read_u32(inbuf + 13);

				struct Op23Result r24 = run_op24(r_ms, hnd, epi, pkt);

				u32 v24[10] = {
					r24.channel, r24.ahb0, r24.segA_base, r24.segB_top,
					r24.segA_top, r24.segB_base, r24.flips,
					r24.bytes_captured, r24.elapsed_ms, r24.error_code
				};
				memset(out, 0, 48);
				out[0] = 24;
				for (int i = 0; i < 10; i++) {
					out[1+i*4+0] = (unsigned char)(v24[i]);
					out[1+i*4+1] = (unsigned char)(v24[i] >> 8);
					out[1+i*4+2] = (unsigned char)(v24[i] >> 16);
					out[1+i*4+3] = (unsigned char)(v24[i] >> 24);
				}
				// 11th u32: ENDPT drain diagnostic, at out[41..44]
				// (within the existing 48-byte response).
				out[41] = (unsigned char)(r24.endpt_diag);
				out[42] = (unsigned char)(r24.endpt_diag >> 8);
				out[43] = (unsigned char)(r24.endpt_diag >> 16);
				out[44] = (unsigned char)(r24.endpt_diag >> 24);
				if (safe_send(client, out, 48)) { closesocket(client); return OP_BREAK; }
	return OP_CONTINUE;
}

static int op_25(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

				// zpod zero-copy iso ring - DIAG (read-only).
				// Request: [1-4]=run_ms [5-8]=handle [9-12]=mode
				//          [13-16]=ring_depth
				// mode 0 = read-only structural dump (cannot wedge):
				//   ENDPTLISTADDR / live dQH[3] / ENDPT* regs /
				//   libnvddk DMA-pool PA·VA·owner / per-ep struct.
				//   The prerequisite for building the ring without
				//   writing the UDC queue head blind.
				u32 r_ms = read_u32(inbuf + 1);
				u32 hnd  = read_u32(inbuf + 5);
				u32 mode = read_u32(inbuf + 9);
				if (hnd == 0) hnd = 0xd3550280u;
				u32 v[48];
				int k;
				for (k = 0; k < 48; k++) v[k] = 0;
				v[0] = 25; v[1] = mode; v[2] = hnd;
				if (mode == 1u) {
					// zero-copy iso ring (Op23Result-shaped reply)
					struct Op23Result rr = run_op25_ring(r_ms, hnd);
					v[3]  = rr.error_code;
					v[4]  = rr.channel;
					v[5]  = rr.ahb0;
					v[6]  = rr.segA_base;          // blk_base
					v[7]  = rr.segB_top;           // gmax
					v[8]  = rr.segA_top;           // dropped (ep1 busy)
					v[9]  = rr.segB_base;          // last AHB_PTR
					v[10] = rr.flips;              // primes
					v[11] = rr.bytes_captured;     // bytes primed
					v[12] = rr.elapsed_ms;
					v[13] = rr.endpt_diag;         // bit0=COMPLETE bit16=STAT
				} else if (mode != 0) {
					v[3] = 0xE0000025u;          // unknown mode
				} else if (!kerncore_is_ready() ||
				           !kerncore_ensure_helpers()) {
					v[3] = 0xE0000002u;
				} else {
					DWORD mmio = kerncore_kreadu32(hnd + 4u);
					v[3] = 0; v[4] = mmio;
					v[5]  = kerncore_kreadu32(mmio + 0x140u); // USBCMD
					v[6]  = kerncore_kreadu32(mmio + 0x144u); // USBSTS
					v[7]  = kerncore_kreadu32(mmio + 0x158u); // ENDPTLISTADDR
					v[8]  = kerncore_kreadu32(mmio + 0x1B0u); // ENDPTPRIME
					v[9]  = kerncore_kreadu32(mmio + 0x1B8u); // ENDPTSTAT
					v[10] = kerncore_kreadu32(mmio + 0x1BCu); // ENDPTCOMPLETE
					v[11] = kerncore_kreadu32(mmio + 0x1C4u); // ENDPTCTRL1
					DWORD eplist = v[7];
					DWORD pg = 0, qoff = 0;
					if (eplist) {
						pg = op23_map(eplist & ~0xFFFu, 0x1000u);
						qoff = eplist & 0xFFFu;
					}
					v[12] = pg;          // raw op23_map result
					v[13] = qoff;
					// dQH[0] (EP0 control = validity oracle: a real
					// mapping shows MaxPktLen=64). v[14..21].
					if (pg)
						for (k = 0; k < 8; k++)
							v[14 + k] = kerncore_kreadu32(pg + qoff + 0u * 0x40u + (DWORD)k * 4u);
					// dQH[3] (ep1 iso). v[22..29].
					if (pg)
						for (k = 0; k < 8; k++)
							v[22 + k] = kerncore_kreadu32(pg + qoff + 3u * 0x40u + (DWORD)k * 4u);
					v[30] = kerncore_kreadu32(hnd + 0x188u); // pool PA
					v[31] = kerncore_kreadu32(hnd + 0x18Cu); // pool VA
					v[32] = kerncore_kreadu32(hnd + 0x190u); // pool owner[0]
					// device-side direct-access test of the 0xd3 band:
					// pool PA head (descriptor), and an unused +0x10000
					// slot region we'd use for the dТD ring (fallback b).
					DWORD ppa = v[30];
					if (ppa) {
						v[33] = kerncore_kreadu32(ppa);
						v[34] = kerncore_kreadu32(ppa + 0x10000u);
					}
					v[35] = kerncore_kreadu32(hnd + 0x7Cu);  // per-ep base
					if (pg) op23_unmap(pg);
				}
				memset(out, 0, 200);
				for (k = 0; k < 48; k++) {
					out[k*4+0] = (unsigned char)(v[k]);
					out[k*4+1] = (unsigned char)(v[k] >> 8);
					out[k*4+2] = (unsigned char)(v[k] >> 16);
					out[k*4+3] = (unsigned char)(v[k] >> 24);
				}
				if (safe_send(client, out, 192)) { closesocket(client); return OP_BREAK; }
	return OP_CONTINUE;
}

static int op_unknown(SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {

out[0] = 0xFF;
				if (safe_send(client,(unsigned char*)out, 32)){
					closesocket(client);
					return OP_BREAK;
				}
			
	return OP_CONTINUE;
}

static const RpcOpEntry g_rpc_ops[] = {
	{1, op_1}, {2, op_2}, {3, op_3}, {4, op_4}, {5, op_5}, {6, op_6},
	{7, op_7}, {8, op_8}, {26, op_26}, {9, op_9}, {10, op_10}, {11, op_11},
	{12, op_12}, {13, op_13}, {14, op_14}, {15, op_15}, {16, op_16}, {17, op_17},
	{18, op_18}, {19, op_19}, {20, op_20}, {21, op_21}, {22, op_22}, {40, op_40},
	{23, op_23}, {24, op_24}, {25, op_25},
};

static int rpc_dispatch(unsigned char op, SOCKET client, unsigned char* inbuf, int res, unsigned char* out) {
	for (unsigned i = 0; i < sizeof(g_rpc_ops)/sizeof(g_rpc_ops[0]); i++)
		if (g_rpc_ops[i].op == op) return g_rpc_ops[i].fn(client, inbuf, res, out);
	return op_unknown(client, inbuf, res, out);
}

void connection(SOCKET client) {
	unsigned char* inbuf = (unsigned char*)calloc(INBUFSZ, 1);
	unsigned char* out = (unsigned char*)calloc(OBUFSZ, 1);
	if (!inbuf || !out) {
		if (inbuf) free(inbuf);
		if (out) free(out);
		closesocket(client);
		return;
	}

	// Goto-cleanup pattern: replaces __try/__finally because the CE 6
	// ARMV4I compiler doesn't support local unwind (C2822). Every
	// early-return path through the body jumps to the `done:` label
	// below; `break` from inside the while-loop falls through to the
	// same label naturally. SEH exceptions still propagate to Server's
	// __except; those will leak inbuf/out, but that's only on actual
	// access-violation crashes which are rare and exceptional.
	{
		char* c = "Hello\n";
		if (send(client,c,strlen(c),0) == SOCKET_ERROR){
			closesocket(client);
			goto done;
		}

		while(true) {
			memset(out, 0, OBUFSZ);

			// Wait up to 30s for the next request. Without this gate,
			// an abandoned client (host crash, USB drop, lost FIN)
			// wedges connection() forever and blocks accept() of the
			// next client. SO_RCVTIMEO does nothing on CE 6.
			int sel = wait_for_readable(client, 30000);
			if (sel <= 0) {
				closesocket(client);
				goto done;
			}

			int res = recv(client,(char*)inbuf,INBUFSZ,0);

			if (res == SOCKET_ERROR || res == 0){
				closesocket(client);
				goto done;
			}

			// read
				if (rpc_dispatch(inbuf[0], client, inbuf, res, out) == OP_BREAK) break;

		}

	}
done:
	free(inbuf);
	free(out);
}

DWORD Server(void* sd_) {
	SOCKADDR_IN addr;
	SOCKET client;
	SOCKET sd = INVALID_SOCKET;
	DWORD bound_iface_sig = 0;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(1337);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	// Outer loop owns the listen-socket lifecycle. We drop back here
	// from two paths:
	//   1. Initial start (sd == INVALID_SOCKET).
	//   2. Interface set changed since bind(). CE 6 binds to the set
	//      of interfaces present at bind() time, so a fresh PPP-over-USB
	//      interface that came up after the daemon started is invisible
	//      to the existing listener. Rebind to pick it up.
	while (!dead) {
		if (sd == INVALID_SOCKET) {
			// Boot + rebind path: keep retrying until bind+listen succeed.
			// At boot the TCP/IP stack may not have accepted INADDR_ANY
			// binds yet (WSAEADDRNOTAVAIL). Sleep + retry until it does.
			sd = socket(AF_INET, SOCK_STREAM, 0);
			if (sd == INVALID_SOCKET) { Sleep(1000); continue; }
			if (bind(sd, (LPSOCKADDR)&addr, sizeof(addr)) == SOCKET_ERROR ||
			    listen(sd, 5) == SOCKET_ERROR) {
				closesocket(sd);
				sd = INVALID_SOCKET;
				Sleep(2000);
				continue;
			}

			bound_iface_sig = compute_iface_signature();
		}

		// select()-with-timeout on the listen socket so we can periodically
		// wake up and check whether the interface set changed (rebind path).
		// SO_RCVTIMEO doesn't fire accept() on CE 6, so this select() is
		// the only mechanism that lets the daemon notice when PPP-over-USB
		// comes up after WiFi. 2s tick is a small CPU cost.
		int sel = wait_for_readable(sd, 2000);

		if (sel == 0) {
			// Idle tick - recompute iface signature, rebind on change.
			DWORD current_sig = compute_iface_signature();
			if (current_sig != bound_iface_sig) {
				closesocket(sd);
				sd = INVALID_SOCKET;
				// Loop back to bind+listen with the new interface set.
			}
			continue;
		}
		if (sel == SOCKET_ERROR) {
			// Listener broken - drop and recreate.
			closesocket(sd);
			sd = INVALID_SOCKET;
			continue;
		}

		client = accept(sd, NULL, NULL);
		if (client == INVALID_SOCKET) continue;

		// Per-connection isolation: an SEH/access-violation inside any
		// opcode handler must not kill the listener. Without this,
		// nativeapp exits and the host loses access until next reboot
		// (zuxhook's SpawnDaemonThread only fires once per ZUxHookInit).
		__try {
			connection(client);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			closesocket(client);
		}
	}

	if (sd != INVALID_SOCKET) closesocket(sd);
	return 1;
}
