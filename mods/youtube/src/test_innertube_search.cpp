// test_innertube_search  (daemon entry: RunDaemon)
//
// Device validation for ce_innertube_search: spawn with --arg "<query>", logs the
// WEB_REMIX search results (videoId / title / artist) to plugin-result-<pid>.log.
// Links ce_https + ce_innertube + wolfSSL + ws2; no COM, no zdksystem.

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "ce_innertube.h"

#define LOG_PATH L"\\flash2\\automation\\itsearch.log"
static void L(const char*s){
    HANDLE f=CreateFileW(LOG_PATH,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return; SetFilePointer(f,0,NULL,FILE_END);
    DWORD n; WriteFile(f,s,(DWORD)strlen(s),&n,NULL); WriteFile(f,"\r\n",2,&n,NULL); CloseHandle(f);
}

extern "C" __declspec(dllexport) int RunDaemon(const void *arg, int arg_len, HANDLE stop_event){
    struct ce_innertube_track tracks[12];
    char query[128]; int n=0, i; enum ce_innertube_result rc;
    char line[600];
    (void)stop_event;

    if(arg && arg_len>0 && arg_len<(int)sizeof(query)){ memcpy(query,arg,arg_len); query[arg_len]=0; }
    else strcpy(query,"daft punk get lucky");

    { char b[180]; _snprintf(b,sizeof(b),"=== ce_innertube_search query=\"%s\" ===",query); L(b); }
    { WSADATA w; WSAStartup(MAKEWORD(2,2),&w); }  // no WSACleanup (daemon-plugin hang pitfall)

    { char contbuf[3072]; contbuf[0]=0;
      rc=ce_innertube_search(query,CE_IT_CAT_SONGS,NULL,tracks,12,&n,contbuf,sizeof(contbuf)); }
    { char b[120]; _snprintf(b,sizeof(b),"result=%s count=%d",ce_innertube_result_str(rc),n); L(b); }
    for(i=0;i<n;i++){
        _snprintf(line,sizeof(line),"  [%d] id=%s vid=%d title=\"%s\" artist=\"%s\"",
                  i,tracks[i].id,tracks[i].is_video,tracks[i].title,tracks[i].artist);
        L(line);
    }
    L("--- done");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h,DWORD r,LPVOID l){(void)h;(void)r;(void)l;return TRUE;}
