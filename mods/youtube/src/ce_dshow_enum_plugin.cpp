// ce_dshow_enum_plugin  (daemon entry: RunDaemon)
//
// RenderFile a working local plain-MP4, then enumerate the resulting filter graph
// (filters + their CLSIDs + each pin's connection media type) to discover the MP4
// parser CLSID and the MEDIATYPE_Stream subtype the source->parser connection uses:
// the subtype our custom IAsyncReader source pin must advertise.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SRC_FILE L"\\flash2\\automation\\ytm-plain.m4a"

static const GUID CLSID_FilterGraph  = {0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IGraphBuilder  = {0x56a868a9,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

struct AM_MEDIA_TYPE { GUID majortype, subtype; int a,b; unsigned long c; GUID formattype; void* pUnk; unsigned long cb; unsigned char* pb; };
struct FILTER_INFO { wchar_t achName[128]; void* pGraph; };
struct PIN_INFO { void* pFilter; int dir; wchar_t achName[128]; };

typedef HRESULT (__stdcall *pfn_CoInitializeEx)(void*, DWORD);
typedef HRESULT (__stdcall *pfn_CoCreateInstance)(const GUID*, void*, DWORD, const GUID*, void**);

static HANDLE g_log;
static void L(const char*s){DWORD n;if(g_log){WriteFile(g_log,s,(DWORD)strlen(s),&n,NULL);WriteFile(g_log,"\r\n",2,&n,NULL);}}
#define VT(o) (*(void***)(o))
static void logguid(const char* tag, const GUID* g){
    char b[160]; _snprintf(b,sizeof(b),"%s {%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x}",
        tag,(unsigned long)g->Data1,g->Data2,g->Data3,g->Data4[0],g->Data4[1],g->Data4[2],g->Data4[3],g->Data4[4],g->Data4[5],g->Data4[6],g->Data4[7]); L(b);
}
static HANDLE open_log(void){ wchar_t p[MAX_PATH]; HANDLE f; _snwprintf(p,MAX_PATH-1,L"\\flash2\\automation\\plugin-result-%lu.log",GetCurrentProcessId()); p[MAX_PATH-1]=0; f=CreateFileW(p,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); if(f==INVALID_HANDLE_VALUE)return NULL; SetFilePointer(f,0,NULL,FILE_END); return f; }

extern "C" __declspec(dllexport) int RunDaemon(const void *arg, int arg_len, HANDLE stop_event){
    HMODULE ole; pfn_CoInitializeEx ci; pfn_CoCreateInstance cci; void* graph=NULL; HRESULT hr;
    (void)arg;(void)arg_len;(void)stop_event;
    g_log=open_log(); if(!g_log) return -1;
    L("=== ce_dshow_enum: dump working RenderFile graph ===");
    ole=LoadLibraryW(L"ole32.dll");
    ci=(pfn_CoInitializeEx)GetProcAddress(ole,L"CoInitializeEx");
    cci=(pfn_CoCreateInstance)GetProcAddress(ole,L"CoCreateInstance");
    if(!cci){ L("no ole32"); CloseHandle(g_log); return 0; }
    if(ci) ci(NULL,0);
    hr=cci(&CLSID_FilterGraph,NULL,1,&IID_IGraphBuilder,&graph);
    if(hr||!graph){ L("no graph"); CloseHandle(g_log); return 0; }
    hr=((HRESULT(__stdcall*)(void*,const wchar_t*,const wchar_t*))VT(graph)[13])(graph,SRC_FILE,NULL); // RenderFile
    { char b[64]; _snprintf(b,sizeof(b),"RenderFile hr=0x%08x",hr); L(b); }

    /* EnumFilters (IFilterGraph vtbl[5]) */
    { void* ef=NULL; hr=((HRESULT(__stdcall*)(void*,void**))VT(graph)[5])(graph,&ef);
      if(hr==0&&ef){
        void* filt=NULL; ULONG got=0;
        while(((HRESULT(__stdcall*)(void*,ULONG,void**,ULONG*))VT(ef)[3])(ef,1,&filt,&got)==0 && got==1){
            GUID clsid; FILTER_INFO fi; char nb[160];
            ((HRESULT(__stdcall*)(void*,GUID*))VT(filt)[3])(filt,&clsid);       // GetClassID
            ((HRESULT(__stdcall*)(void*,FILTER_INFO*))VT(filt)[12])(filt,&fi);   // QueryFilterInfo
            { int i; for(i=0;fi.achName[i]&&i<120;i++) nb[i]=(char)fi.achName[i]; nb[i]=0; }
            { char b[200]; _snprintf(b,sizeof(b),"FILTER '%s'",nb); L(b); }
            logguid("  clsid",&clsid);
            if(fi.pGraph) ((ULONG(__stdcall*)(void*))VT(fi.pGraph)[2])(fi.pGraph);
            /* EnumPins (IBaseFilter vtbl[10]) */
            { void* ep=NULL;
              if(((HRESULT(__stdcall*)(void*,void**))VT(filt)[10])(filt,&ep)==0&&ep){
                void* pin=NULL; ULONG pg=0;
                while(((HRESULT(__stdcall*)(void*,ULONG,void**,ULONG*))VT(ep)[3])(ep,1,&pin,&pg)==0 && pg==1){
                    int dir=-1; void* peer=NULL;
                    ((HRESULT(__stdcall*)(void*,int*))VT(pin)[9])(pin,&dir);           // QueryDirection
                    if(((HRESULT(__stdcall*)(void*,void**))VT(pin)[6])(pin,&peer)==0&&peer){ // ConnectedTo
                        AM_MEDIA_TYPE mt; memset(&mt,0,sizeof(mt));
                        if(((HRESULT(__stdcall*)(void*,AM_MEDIA_TYPE*))VT(pin)[7])(pin,&mt)==0){ // ConnectionMediaType
                            char b[300]; _snprintf(b,sizeof(b),"  pin dir=%d CONNECTED:",dir); L(b);
                            logguid("    major",&mt.majortype); logguid("    subtype",&mt.subtype);
                            logguid("    formattype",&mt.formattype);
                            _snprintf(b,sizeof(b),"    fixed=%d sampleSize=%lu cbFormat=%lu",mt.a,mt.c,mt.cb); L(b);
                            if(mt.pb && mt.cb>0){ unsigned long i; char *h=b; h+=_snprintf(h,16,"    pb=");
                                for(i=0;i<mt.cb && i<96;i++) h+=_snprintf(h,4,"%02x",mt.pb[i]); L(b); }
                        }
                        ((ULONG(__stdcall*)(void*))VT(peer)[2])(peer);
                    }
                    ((ULONG(__stdcall*)(void*))VT(pin)[2])(pin); pin=NULL;
                }
                ((ULONG(__stdcall*)(void*))VT(ep)[2])(ep);
              }
            }
            ((ULONG(__stdcall*)(void*))VT(filt)[2])(filt); filt=NULL;
        }
        ((ULONG(__stdcall*)(void*))VT(ef)[2])(ef);
      }
    }
    L("--- enum exit");
    ((ULONG(__stdcall*)(void*))VT(graph)[2])(graph);
    CloseHandle(g_log);
    return 0;
}
extern "C" BOOL WINAPI DllMain(HANDLE h,DWORD r,LPVOID l){(void)h;(void)r;(void)l;return TRUE;}
