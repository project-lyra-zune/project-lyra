// ce_dshow_scheme_plugin  (daemon entry: RunDaemon)
//
// native-queue-branch step-2 confirm probe. Tests whether CE's IGraphBuilder::
// RenderFile resolves a CUSTOM URL scheme via the registry the same way it does
// for http (HKCR\http\Source Filter = {zwmtstreamer}). If it does, registering
// our own source for a `ytm` scheme is the clean branch (no in-proc hook).
//
// The probe registers (RAM hive, torn down on exit):
//   HKCR\ytm\Source Filter      = {CLSID_OURS}   (+ URL Protocol)
//   HKCR\CLSID\{CLSID_OURS}\(Default), Merit
//   HKCR\CLSID\{CLSID_OURS}\InprocServer32\(Default) = this DLL path, ThreadingModel=Both
// then CoCreateInstance(FilterGraph) -> RenderFile(L"ytm://testid12345").
//
// PROOF SIGNALS (logged): DllGetClassObject fired, IClassFactory::CreateInstance
// fired, IFileSourceFilter::Load fired (with the ytm:// URL). If Load fires, CE
// honours custom-scheme Source-Filter registration -> scheme registration viable.
//
// Spawn via nativeapp opcode 21 (entry "RunDaemon"). Log: plugin-result-<pid>.log.
// The DLL must live at the InprocServer32 path below so COM reuses the loaded image.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define DLL_PATH    L"\\flash2\\automation\\ce_dshow_scheme_plugin.dll"
#define TEST_SCHEME L"ytm"
#define TEST_URL    L"ytm://testid12345"
#define CLSID_STR   L"{C0FFEE01-2026-4A6E-B0DE-5965594A5959}"

// ── GUIDs ──────────────────────────────────────────────────────────────────
static const GUID IID_IUnknown_        = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IClassFactory_   = {0x00000001,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IPersist_        = {0x0000010c,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IMediaFilter_    = {0x56a86899,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IBaseFilter_     = {0x56a86895,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IFileSourceFilter_={0x56a868a6,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IPin_            = {0x56a86891,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IEnumPins_       = {0x56a86892,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IEnumMediaTypes_ = {0x89c31040,0x846b,0x11ce,{0x97,0xd3,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static const GUID MEDIATYPE_Stream_    = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID GUID_NULL_           = {0,0,0,{0,0,0,0,0,0,0,0}};
static const GUID CLSID_OURS           = {0xC0FFEE01,0x2026,0x4A6E,{0xB0,0xDE,0x59,0x65,0x59,0x4A,0x59,0x59}};
static const GUID CLSID_FilterGraph    = {0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IGraphBuilder    = {0x56a868a9,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

typedef HRESULT (__stdcall *pfn_CoInitializeEx)(void*, DWORD);
typedef HRESULT (__stdcall *pfn_CoCreateInstance)(const GUID*, void*, DWORD, const GUID*, void**);

struct AM_MEDIA_TYPE {
    GUID majortype, subtype; int bFixedSizeSamples, bTemporalCompression;
    unsigned long lSampleSize; GUID formattype; void* pUnk; unsigned long cbFormat; unsigned char* pbFormat;
};
struct PIN_INFO { void* pFilter; int dir; wchar_t achName[128]; };
struct FILTER_INFO { wchar_t achName[128]; void* pGraph; };

static HANDLE g_log;
static int g_hit_dllgco=0, g_hit_createinst=0, g_hit_load=0;
static wchar_t g_load_url[260]={0};

static void L(const char*s){DWORD n;if(g_log){WriteFile(g_log,s,(DWORD)strlen(s),&n,NULL);WriteFile(g_log,"\r\n",2,&n,NULL);}}
static void Lx(const char*tag,HRESULT hr){char b[160];_snprintf(b,sizeof(b),"%s hr=0x%08x",tag,hr);L(b);}
static int guid_eq(const GUID*a,const GUID*b){return memcmp(a,b,16)==0;}

#define VT(o) (*(void***)(o))
static HRESULT g_QI(void* o, const GUID* iid, void** out){ return ((HRESULT(__stdcall*)(void*,const GUID*,void**))VT(o)[0])(o,iid,out); }
static ULONG  g_REL(void* o){ return ((ULONG(__stdcall*)(void*))VT(o)[2])(o); }

static void* (__stdcall *g_CoTaskMemAlloc)(SIZE_T);

// ── objects: Filter (IBaseFilter + IFileSourceFilter) + one output Pin ───────
struct Pin {
    void* vPin; LONG ref; struct Filter* filter; void* peer; AM_MEDIA_TYPE mt;
};
struct Filter {
    void* vFilter;   // IBaseFilter      (offset 0)
    void* vFSF;      // IFileSourceFilter(offset sizeof(ptr))
    LONG  ref; Pin* pin; void* graph; int state; wchar_t curfile[260];
};
static Filter* FSF_to_Filter(void* o){ return (Filter*)((char*)o - sizeof(void*)); }

static void* g_FilterVtbl[15];
static void* g_FSFVtbl[5];
static void* g_PinVtbl[18];
static void* g_EnumMTVtbl[7];
static void* g_EnumPinsVtbl[7];
static void* g_CFVtbl[5];

static AM_MEDIA_TYPE* CoTaskMT(){ return (AM_MEDIA_TYPE*)g_CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)); }

// ---- forward decls ----
static HRESULT __stdcall F_QI(void* o,const GUID* iid,void** out);

// ---- IPin (minimal output pin offering MEDIATYPE_Stream) ----
static HRESULT __stdcall Pin_QI(void* o,const GUID* iid,void** out){
    Pin* p=(Pin*)o;
    if(guid_eq(iid,&IID_IUnknown_)||guid_eq(iid,&IID_IPin_)){ *out=&p->vPin; InterlockedIncrement(&p->ref); return 0; }
    *out=NULL; return 0x80004002;
}
static ULONG __stdcall Pin_AddRef(void* o){ return InterlockedIncrement(&((Pin*)o)->ref); }
static ULONG __stdcall Pin_Release(void* o){ return InterlockedDecrement(&((Pin*)o)->ref); }
static HRESULT __stdcall Pin_Connect(void* o,void* recv,const AM_MEDIA_TYPE* pmt){(void)o;(void)recv;(void)pmt; return 0x80004005; }
static HRESULT __stdcall Pin_ReceiveConnection(void* o,void* c,const AM_MEDIA_TYPE* m){(void)o;(void)c;(void)m; return 0x80004001; }
static HRESULT __stdcall Pin_Disconnect(void* o){ ((Pin*)o)->peer=NULL; return 0; }
static HRESULT __stdcall Pin_ConnectedTo(void* o,void** pp){ Pin* p=(Pin*)o; if(!p->peer){*pp=NULL;return 0x80040209;} *pp=p->peer; return 0; }
static HRESULT __stdcall Pin_ConnectionMediaType(void* o,AM_MEDIA_TYPE* m){ Pin* p=(Pin*)o; *m=p->mt; return 0; }
static HRESULT __stdcall Pin_QueryPinInfo(void* o,PIN_INFO* info){ Pin* p=(Pin*)o; info->pFilter=&p->filter->vFilter; ((ULONG(__stdcall*)(void*))g_FilterVtbl[1])(&p->filter->vFilter); info->dir=1; wcscpy(info->achName,L"Output"); return 0; }
static HRESULT __stdcall Pin_QueryDirection(void* o,int* d){(void)o; *d=1; return 0; }
static HRESULT __stdcall Pin_QueryId(void* o,wchar_t** id){(void)o; *id=NULL; return 0x80004001; }
static HRESULT __stdcall Pin_QueryAccept(void* o,const AM_MEDIA_TYPE* m){ (void)o; return guid_eq(&m->majortype,&MEDIATYPE_Stream_)?0:1; }
static HRESULT __stdcall Pin_EnumMediaTypes(void* o,void** e);
static HRESULT __stdcall Pin_QueryInternalConnections(void* o,void** a,ULONG* n){(void)o;(void)a; if(n)*n=0; return 0; }
static HRESULT __stdcall Pin_EndOfStream(void* o){(void)o;return 0;}
static HRESULT __stdcall Pin_BeginFlush(void* o){(void)o;return 0;}
static HRESULT __stdcall Pin_EndFlush(void* o){(void)o;return 0;}
static HRESULT __stdcall Pin_NewSegment(void* o,LONGLONG a,LONGLONG b,double r){(void)o;(void)a;(void)b;(void)r;return 0;}

struct EnumMT { void* v; LONG ref; Pin* pin; int idx; };
static HRESULT __stdcall EMT_QI(void* o,const GUID* iid,void** out){ EnumMT* e=(EnumMT*)o; if(guid_eq(iid,&IID_IUnknown_)||guid_eq(iid,&IID_IEnumMediaTypes_)){*out=o;InterlockedIncrement(&e->ref);return 0;} *out=NULL; return 0x80004002; }
static ULONG __stdcall EMT_AddRef(void* o){ return InterlockedIncrement(&((EnumMT*)o)->ref); }
static ULONG __stdcall EMT_Release(void* o){ EnumMT* e=(EnumMT*)o; LONG r=InterlockedDecrement(&e->ref); if(r==0) LocalFree(e); return r; }
static HRESULT __stdcall EMT_Next(void* o,ULONG c,AM_MEDIA_TYPE** out,ULONG* got){
    EnumMT* e=(EnumMT*)o; ULONG n=0;
    while(n<c && e->idx<1){ AM_MEDIA_TYPE* m=CoTaskMT(); if(!m)break; *m=e->pin->mt; out[n]=m; n++; e->idx++; }
    if(got)*got=n; return n==c?0:1;
}
static HRESULT __stdcall EMT_Skip(void* o,ULONG c){ ((EnumMT*)o)->idx+=c; return 0; }
static HRESULT __stdcall EMT_Reset(void* o){ ((EnumMT*)o)->idx=0; return 0; }
static HRESULT __stdcall EMT_Clone(void* o,void** pp){(void)o;*pp=NULL;return 0x80004001;}
static HRESULT __stdcall Pin_EnumMediaTypes(void* o,void** e){
    Pin* p=(Pin*)o; EnumMT* em=(EnumMT*)LocalAlloc(LPTR,sizeof(EnumMT));
    if(!em)return 0x8007000e; em->v=g_EnumMTVtbl; em->ref=1; em->pin=p; em->idx=0; *e=em; return 0;
}

struct EnumPins { void* v; LONG ref; Filter* filter; int idx; };
static HRESULT __stdcall EP_QI(void* o,const GUID* iid,void** out){ EnumPins* e=(EnumPins*)o; if(guid_eq(iid,&IID_IUnknown_)||guid_eq(iid,&IID_IEnumPins_)){*out=o;InterlockedIncrement(&e->ref);return 0;} *out=NULL; return 0x80004002; }
static ULONG __stdcall EP_AddRef(void* o){ return InterlockedIncrement(&((EnumPins*)o)->ref); }
static ULONG __stdcall EP_Release(void* o){ EnumPins* e=(EnumPins*)o; LONG r=InterlockedDecrement(&e->ref); if(r==0) LocalFree(e); return r; }
static HRESULT __stdcall EP_Next(void* o,ULONG c,void** out,ULONG* got){
    EnumPins* e=(EnumPins*)o; ULONG n=0;
    while(n<c && e->idx<1){ Pin* p=e->filter->pin; out[n]=&p->vPin; InterlockedIncrement(&p->ref); n++; e->idx++; }
    if(got)*got=n; return n==c?0:1;
}
static HRESULT __stdcall EP_Skip(void* o,ULONG c){ ((EnumPins*)o)->idx+=c; return 0; }
static HRESULT __stdcall EP_Reset(void* o){ ((EnumPins*)o)->idx=0; return 0; }
static HRESULT __stdcall EP_Clone(void* o,void** pp){(void)o;*pp=NULL;return 0x80004001;}

// ---- IFileSourceFilter (the probe target) ----
static HRESULT __stdcall FSF_QI(void* o,const GUID* iid,void** out){ Filter* f=FSF_to_Filter(o); return F_QI(&f->vFilter,iid,out); }
static ULONG __stdcall FSF_AddRef(void* o){ return InterlockedIncrement(&FSF_to_Filter(o)->ref); }
static ULONG __stdcall FSF_Release(void* o){ return InterlockedDecrement(&FSF_to_Filter(o)->ref); }
static HRESULT __stdcall FSF_Load(void* o,const wchar_t* name,const AM_MEDIA_TYPE* pmt){
    Filter* f=FSF_to_Filter(o); (void)pmt;
    g_hit_load=1;
    if(name){ wcsncpy(g_load_url,name,259); g_load_url[259]=0; wcsncpy(f->curfile,name,259); }
    { char b[320]; char u[260]; int i=0; if(name){ while(name[i]&&i<259){u[i]=(char)name[i];i++;} } u[i]=0;
      _snprintf(b,sizeof(b),">>> IFileSourceFilter::Load url=\"%s\"  (SCHEME REGISTRATION HONOURED)",u); L(b); }
    return 0;
}
static HRESULT __stdcall FSF_GetCurFile(void* o,wchar_t** ppName,AM_MEDIA_TYPE* pmt){ Filter* f=FSF_to_Filter(o); (void)pmt; if(ppName)*ppName=f->curfile; return 0; }

// ---- IBaseFilter ----
static HRESULT __stdcall F_QI(void* o,const GUID* iid,void** out){
    Filter* f=(Filter*)o;
    if(guid_eq(iid,&IID_IUnknown_)||guid_eq(iid,&IID_IPersist_)||guid_eq(iid,&IID_IMediaFilter_)||guid_eq(iid,&IID_IBaseFilter_)){ *out=&f->vFilter; InterlockedIncrement(&f->ref); return 0; }
    if(guid_eq(iid,&IID_IFileSourceFilter_)){ *out=&f->vFSF; InterlockedIncrement(&f->ref); L("F_QI(IFileSourceFilter) -> ok"); return 0; }
    *out=NULL; return 0x80004002;
}
static ULONG __stdcall F_AddRef(void* o){ return InterlockedIncrement(&((Filter*)o)->ref); }
static ULONG __stdcall F_Release(void* o){ Filter* f=(Filter*)o; LONG r=InterlockedDecrement(&f->ref); if(r==0){ if(f->pin)LocalFree(f->pin); LocalFree(f);} return r; }
static HRESULT __stdcall F_GetClassID(void* o,GUID* id){(void)o; *id=CLSID_OURS; return 0; }
static HRESULT __stdcall F_Stop(void* o){ ((Filter*)o)->state=0; return 0; }
static HRESULT __stdcall F_Pause(void* o){ ((Filter*)o)->state=1; return 0; }
static HRESULT __stdcall F_Run(void* o,LONGLONG t){(void)t; ((Filter*)o)->state=2; return 0; }
static HRESULT __stdcall F_GetState(void* o,DWORD ms,int* st){(void)ms; *st=((Filter*)o)->state; return 0; }
static HRESULT __stdcall F_SetSyncSource(void* o,void* c){(void)o;(void)c;return 0;}
static HRESULT __stdcall F_GetSyncSource(void* o,void** c){(void)o;*c=NULL;return 0;}
static HRESULT __stdcall F_EnumPins(void* o,void** e){
    Filter* f=(Filter*)o; EnumPins* ep=(EnumPins*)LocalAlloc(LPTR,sizeof(EnumPins));
    if(!ep)return 0x8007000e; ep->v=g_EnumPinsVtbl; ep->ref=1; ep->filter=f; ep->idx=0; *e=ep; return 0;
}
static HRESULT __stdcall F_FindPin(void* o,const wchar_t* id,void** pp){(void)o;(void)id;*pp=NULL;return 0x80004005;}
static HRESULT __stdcall F_QueryFilterInfo(void* o,FILTER_INFO* info){ Filter* f=(Filter*)o; wcscpy(info->achName,L"YtmSchemeProbe"); info->pGraph=f->graph; if(f->graph)((ULONG(__stdcall*)(void*))VT(f->graph)[1])(f->graph); return 0; }
static HRESULT __stdcall F_JoinFilterGraph(void* o,void* g,const wchar_t* n){(void)n; ((Filter*)o)->graph=g; return 0; }
static HRESULT __stdcall F_QueryVendorInfo(void* o,wchar_t** v){(void)o;*v=NULL;return 0x80004001;}

static Filter* make_filter(){
    Filter* f=(Filter*)LocalAlloc(LPTR,sizeof(Filter));
    Pin* p=(Pin*)LocalAlloc(LPTR,sizeof(Pin));
    if(!f||!p){ if(f)LocalFree(f); if(p)LocalFree(p); return NULL; }
    f->vFilter=g_FilterVtbl; f->vFSF=g_FSFVtbl; f->ref=1; f->pin=p; f->graph=NULL; f->state=0; f->curfile[0]=0;
    p->vPin=g_PinVtbl; p->ref=1; p->filter=f; p->peer=NULL;
    memset(&p->mt,0,sizeof(p->mt));
    p->mt.majortype=MEDIATYPE_Stream_; p->mt.subtype=GUID_NULL_; p->mt.bFixedSizeSamples=1; p->mt.lSampleSize=1; p->mt.formattype=GUID_NULL_;
    return f;
}

// ---- IClassFactory ----
struct ClassFactory { void* v; LONG ref; };
static ClassFactory g_factory;
static HRESULT __stdcall CF_QI(void* o,const GUID* iid,void** out){ if(guid_eq(iid,&IID_IUnknown_)||guid_eq(iid,&IID_IClassFactory_)){*out=o; return 0;} *out=NULL; return 0x80004002; }
static ULONG __stdcall CF_AddRef(void* o){ return InterlockedIncrement(&((ClassFactory*)o)->ref); }
static ULONG __stdcall CF_Release(void* o){ return InterlockedDecrement(&((ClassFactory*)o)->ref); }
static HRESULT __stdcall CF_CreateInstance(void* o,void* outer,const GUID* iid,void** ppv){
    (void)o;(void)outer; g_hit_createinst=1; L(">>> IClassFactory::CreateInstance fired");
    Filter* f=make_filter(); if(!f){*ppv=NULL;return 0x8007000e;}
    HRESULT hr=F_QI(&f->vFilter,iid,ppv);
    F_Release(&f->vFilter); // QI took its own ref
    return hr;
}
static HRESULT __stdcall CF_LockServer(void* o,BOOL f){(void)o;(void)f;return 0;}

static void init_vtbls(){
    void* F[15]={(void*)F_QI,(void*)F_AddRef,(void*)F_Release,(void*)F_GetClassID,(void*)F_Stop,(void*)F_Pause,(void*)F_Run,(void*)F_GetState,(void*)F_SetSyncSource,(void*)F_GetSyncSource,(void*)F_EnumPins,(void*)F_FindPin,(void*)F_QueryFilterInfo,(void*)F_JoinFilterGraph,(void*)F_QueryVendorInfo};
    void* S[5]={(void*)FSF_QI,(void*)FSF_AddRef,(void*)FSF_Release,(void*)FSF_Load,(void*)FSF_GetCurFile};
    void* P[18]={(void*)Pin_QI,(void*)Pin_AddRef,(void*)Pin_Release,(void*)Pin_Connect,(void*)Pin_ReceiveConnection,(void*)Pin_Disconnect,(void*)Pin_ConnectedTo,(void*)Pin_ConnectionMediaType,(void*)Pin_QueryPinInfo,(void*)Pin_QueryDirection,(void*)Pin_QueryId,(void*)Pin_QueryAccept,(void*)Pin_EnumMediaTypes,(void*)Pin_QueryInternalConnections,(void*)Pin_EndOfStream,(void*)Pin_BeginFlush,(void*)Pin_EndFlush,(void*)Pin_NewSegment};
    void* EM[7]={(void*)EMT_QI,(void*)EMT_AddRef,(void*)EMT_Release,(void*)EMT_Next,(void*)EMT_Skip,(void*)EMT_Reset,(void*)EMT_Clone};
    void* EP[7]={(void*)EP_QI,(void*)EP_AddRef,(void*)EP_Release,(void*)EP_Next,(void*)EP_Skip,(void*)EP_Reset,(void*)EP_Clone};
    void* CF[5]={(void*)CF_QI,(void*)CF_AddRef,(void*)CF_Release,(void*)CF_CreateInstance,(void*)CF_LockServer};
    memcpy(g_FilterVtbl,F,sizeof(F)); memcpy(g_FSFVtbl,S,sizeof(S)); memcpy(g_PinVtbl,P,sizeof(P));
    memcpy(g_EnumMTVtbl,EM,sizeof(EM)); memcpy(g_EnumPinsVtbl,EP,sizeof(EP)); memcpy(g_CFVtbl,CF,sizeof(CF));
    g_factory.v=g_CFVtbl; g_factory.ref=1;
}

// ── COM server exports ───────────────────────────────────────────────────────
// exported via the .def (objbase.h already prototypes these without dllexport)
STDAPI DllGetClassObject(REFCLSID rclsid,REFIID riid,void** ppv){
    g_hit_dllgco=1; L(">>> DllGetClassObject fired");
    if(guid_eq(&rclsid,&CLSID_OURS) && (guid_eq(&riid,&IID_IClassFactory_)||guid_eq(&riid,&IID_IUnknown_))){
        *ppv=&g_factory; return 0;
    }
    *ppv=NULL; return 0x80040111; // CLASS_E_CLASSNOTAVAILABLE
}
STDAPI DllCanUnloadNow(void){ return 1; /* S_FALSE: keep loaded */ }

// ── registry setup / teardown (HKCR, RAM hive, no persistence) ──────────────
static void reg_setup(){
    HKEY k; DWORD disp; DWORD merit=0x800002;
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,TEST_SCHEME,0,NULL,0,0xF003F/*KEY_ALL_ACCESS*/,NULL,&k,&disp)==0){
        RegSetValueExW(k,L"Source Filter",0,REG_SZ,(BYTE*)CLSID_STR,(DWORD)((wcslen(CLSID_STR)+1)*2));
        RegSetValueExW(k,L"URL Protocol",0,REG_SZ,(BYTE*)L"",2);
        RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR,0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)L"Xune YT Scheme Probe",(DWORD)((wcslen(L"Xune YT Scheme Probe")+1)*2));
        RegSetValueExW(k,L"Merit",0,REG_DWORD,(BYTE*)&merit,4);
        RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR L"\\InprocServer32",0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)DLL_PATH,(DWORD)((wcslen(DLL_PATH)+1)*2));
        RegSetValueExW(k,L"ThreadingModel",0,REG_SZ,(BYTE*)L"Both",10);
        RegCloseKey(k);
    }
}
static void reg_teardown(){
    RegDeleteKeyW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR L"\\InprocServer32");
    RegDeleteKeyW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR);
    RegDeleteKeyW(HKEY_CLASSES_ROOT,TEST_SCHEME);
}

static HANDLE open_log(void){ wchar_t p[MAX_PATH]; HANDLE f; _snwprintf(p,MAX_PATH-1,L"\\flash2\\automation\\plugin-result-%lu.log",GetCurrentProcessId()); p[MAX_PATH-1]=0; f=CreateFileW(p,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); if(f==INVALID_HANDLE_VALUE)return NULL; SetFilePointer(f,0,NULL,FILE_END); return f; }

extern "C" __declspec(dllexport) int RunDaemon(const void *arg, int arg_len, HANDLE stop_event){
    char line[200];
    HMODULE ole; pfn_CoInitializeEx CoInitializeEx_; pfn_CoCreateInstance CoCreateInstance_;
    void *graph=NULL; HRESULT hr;
    (void)arg;(void)arg_len;(void)stop_event;

    g_log=open_log(); if(!g_log) return -1;
    L("=== ce_dshow_scheme (daemon): RenderFile custom-scheme registration probe ===");
    init_vtbls();

    ole=LoadLibraryW(L"ole32.dll");
    CoInitializeEx_   = ole?(pfn_CoInitializeEx)GetProcAddress(ole,L"CoInitializeEx"):NULL;
    CoCreateInstance_ = ole?(pfn_CoCreateInstance)GetProcAddress(ole,L"CoCreateInstance"):NULL;
    g_CoTaskMemAlloc  = ole?(void*(__stdcall*)(SIZE_T))GetProcAddress(ole,L"CoTaskMemAlloc"):NULL;
    if(!CoCreateInstance_||!g_CoTaskMemAlloc){ L("ole32 resolve failed"); CloseHandle(g_log); return 0; }
    if(CoInitializeEx_) CoInitializeEx_(NULL,0);

    reg_setup();
    L("registry written: HKCR\\ytm\\Source Filter + HKCR\\CLSID\\{ours}\\InprocServer32");

    hr=CoCreateInstance_(&CLSID_FilterGraph,NULL,1,&IID_IGraphBuilder,&graph);
    Lx("CoCreateInstance(FilterGraph)",hr); if(hr||!graph){ reg_teardown(); CloseHandle(g_log); return 0; }

    L("calling RenderFile(L\"ytm://testid12345\") ...");
    hr=((HRESULT(__stdcall*)(void*,const wchar_t*,const wchar_t*))VT(graph)[13])(graph,TEST_URL,NULL);
    Lx("RenderFile(ytm://...)",hr);

    L("---- RESULT ----");
    _snprintf(line,sizeof(line),"DllGetClassObject fired = %d", g_hit_dllgco); L(line);
    _snprintf(line,sizeof(line),"ClassFactory::CreateInstance fired = %d", g_hit_createinst); L(line);
    _snprintf(line,sizeof(line),"IFileSourceFilter::Load fired = %d", g_hit_load); L(line);
    if(g_hit_load){ char u[260]; int i=0; while(g_load_url[i]&&i<259){u[i]=(char)g_load_url[i];i++;} u[i]=0; _snprintf(line,sizeof(line),"Load url = \"%s\"",u); L(line); }
    L(g_hit_load ? "VERDICT: SCHEME REGISTRATION HONOURED -> register-our-source branch viable"
                 : "VERDICT: scheme not honoured by RenderFile (see hr above) -> need hook or different reg");

    if(graph) g_REL(graph);
    reg_teardown();
    L("--- exit");
    CloseHandle(g_log);
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h,DWORD r,LPVOID l){(void)h;(void)r;(void)l;return TRUE;}
