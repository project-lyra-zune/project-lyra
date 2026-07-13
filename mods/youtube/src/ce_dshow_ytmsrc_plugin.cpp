// ce_dshow_ytmsrc_plugin  (daemon entry: RunDaemon)
//
// native-queue-branch step 3 / directshow-stream milestone 3a: the REAL ytm://
// source, registered for the `ytm` scheme (device-proven mechanism). RenderFile
// CoCreates it via HKCR\ytm\Source Filter -> IFileSourceFilter::Load(ytm://<id>),
// then connects our output pin (AAC type b0813207/df5af35b) into the native AAC
// decoder {66556a5a} -> renderer {0688722c}. Our worker thread (started on
// Filter::Pause) pushes the demuxed AAC AUs clock-paced -> audible.
//
// 3a backs Load with a LOCAL de-fragged file (ytm://NAME -> \flash2\automation\
// NAME.m4a; parse stsz/stco). 3b swaps that backend for ce_https fetch + on-the-
// fly de-frag. Merges the scheme-probe COM-server registration with the push
// filter's AAC-into-native-decoder machinery.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define DLL_PATH    L"\\flash2\\automation\\ce_ytmsrc5.dll"
#define TEST_SCHEME L"ytm"
#define CLSID_STR   L"{C0FFEE05-2026-4A6E-B0DE-5965594A5959}"
#define TEST_URL    L"ytm://ytm-plain"

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
static const GUID IID_IMemInputPin_    = {0x56a8689d,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID MT_major  = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}}; // MEDIATYPE_Audio (0x160a config-fill chain)
static const GUID MT_sub    = {0x0000160a,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}}; // AAC (Receive path: config from WAVEFORMATEX)
static const GUID FORMAT_WaveFormatEx_ = {0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static const GUID GUID_NULL_ = {0,0,0,{0,0,0,0,0,0,0,0}};
static const GUID CLSID_OURS = {0xC0FFEE05,0x2026,0x4A6E,{0xB0,0xDE,0x59,0x65,0x59,0x4A,0x59,0x59}};
static const GUID CLSID_FilterGraph = {0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IGraphBuilder = {0x56a868a9,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IMediaControl = {0x56a868b1,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

typedef HRESULT (__stdcall *pfn_CoInitializeEx)(void*, DWORD);
typedef HRESULT (__stdcall *pfn_CoCreateInstance)(const GUID*, void*, DWORD, const GUID*, void**);

struct AM_MEDIA_TYPE { GUID majortype, subtype; int bFixed, bTemporal; unsigned long lSampleSize; GUID formattype; void* pUnk; unsigned long cbFormat; unsigned char* pbFormat; };
struct PIN_INFO { void* pFilter; int dir; wchar_t achName[128]; };
struct FILTER_INFO { wchar_t achName[128]; void* pGraph; };
struct ALLOC_PROPS { long cBuffers, cbBuffer, cbAlign, cbPrefix; };

static int g_objcount=0;       // live COM instances -> gates DllCanUnloadNow
static void* (__stdcall *g_CoTaskMemAlloc)(SIZE_T);
static unsigned char g_wfx[18] = {0x0a,0x16, 2,0, 0x44,0xac,0,0, 0,0,0,0, 0,0, 0,0, 0,0}; // tag0x160a,2ch,44100,cbSize0
static int g_hit_dllgco=0, g_hit_createinst=0, g_hit_load=0, g_hit_connect=0;

// Per-line open-append-close log: never holds the handle, so the file is always
// readable even while the COM source lives inside long-running servicesd. Each
// process logs to its own pid-named file.
static void L(const char*s){
    wchar_t p[MAX_PATH]; _snwprintf(p,MAX_PATH-1,L"\\flash2\\automation\\plugin-result-%lu.log",GetCurrentProcessId()); p[MAX_PATH-1]=0;
    HANDLE f=CreateFileW(p,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return; SetFilePointer(f,0,NULL,FILE_END);
    DWORD n; WriteFile(f,s,(DWORD)strlen(s),&n,NULL); WriteFile(f,"\r\n",2,&n,NULL); CloseHandle(f);
}
static void Lx(const char*t,HRESULT hr){char b[160];_snprintf(b,sizeof(b),"%s hr=0x%08x",t,hr);L(b);}
static int geq(const GUID*a,const GUID*b){return memcmp(a,b,16)==0;}
#define VT(o) (*(void***)(o))
static unsigned rd32(const unsigned char*p){return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];}

static void fill_mt(AM_MEDIA_TYPE* m){
    memset(m,0,sizeof(*m));
    m->majortype=MT_major; m->subtype=MT_sub; m->bFixed=0; m->lSampleSize=1;
    m->formattype=FORMAT_WaveFormatEx_; m->cbFormat=18; m->pbFormat=g_wfx;
}

// ── objects: Filter (IBaseFilter + IFileSourceFilter) + push output Pin ──────
struct Pin { void* v; LONG ref; struct Filter* filter; void* peer; void* meminput; void* alloc; AM_MEDIA_TYPE mt; };
struct Filter {
    void* vFilter;   // IBaseFilter       (offset 0)
    void* vFSF;      // IFileSourceFilter (offset sizeof(ptr))
    LONG ref; Pin* pin; void* graph; int state;
    unsigned* sizes; unsigned nsamp, base; HANDLE srcf, hThread, hStop; wchar_t curfile[260];
};
static Filter* FSF_to_Filter(void* o){ return (Filter*)((char*)o - sizeof(void*)); }
static void* g_FVtbl[15]; static void* g_FSFVtbl[5]; static void* g_PVtbl[18]; static void* g_EPVtbl[7]; static void* g_EMVtbl[7]; static void* g_CFVtbl[5];
static DWORD WINAPI push_worker(LPVOID param);
static HRESULT __stdcall F_QI(void*o,const GUID*i,void**out);

// IEnumMediaTypes
struct EnumMT { void* v; LONG ref; int idx; };
static AM_MEDIA_TYPE* mt_alloc(){
    if(!g_CoTaskMemAlloc){ HMODULE ole=LoadLibraryW(L"ole32.dll"); if(ole) g_CoTaskMemAlloc=(void*(__stdcall*)(SIZE_T))GetProcAddress(ole,L"CoTaskMemAlloc"); }
    if(!g_CoTaskMemAlloc) return NULL;
    AM_MEDIA_TYPE* m=(AM_MEDIA_TYPE*)g_CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)); if(m){ unsigned char* pb=(unsigned char*)g_CoTaskMemAlloc(18); fill_mt(m); if(pb){memcpy(pb,g_wfx,18); m->pbFormat=pb;} } return m; }
static HRESULT __stdcall EM_QI(void*o,const GUID*i,void**out){EnumMT*e=(EnumMT*)o;if(geq(i,&IID_IUnknown_)||geq(i,&IID_IEnumMediaTypes_)){*out=o;InterlockedIncrement(&e->ref);return 0;}*out=0;return 0x80004002;}
static ULONG __stdcall EM_AR(void*o){return InterlockedIncrement(&((EnumMT*)o)->ref);}
static ULONG __stdcall EM_RL(void*o){EnumMT*e=(EnumMT*)o;LONG r=InterlockedDecrement(&e->ref);if(!r)LocalFree(e);return r;}
static HRESULT __stdcall EM_Next(void*o,ULONG c,AM_MEDIA_TYPE**out,ULONG*got){EnumMT*e=(EnumMT*)o;ULONG n=0;while(n<c&&e->idx<1){AM_MEDIA_TYPE*m=mt_alloc();if(!m)break;out[n]=m;n++;e->idx++;}if(got)*got=n;return n==c?0:1;}
static HRESULT __stdcall EM_Skip(void*o,ULONG c){((EnumMT*)o)->idx+=c;return 0;}
static HRESULT __stdcall EM_Reset(void*o){((EnumMT*)o)->idx=0;return 0;}
static HRESULT __stdcall EM_Clone(void*o,void**p){(void)o;*p=0;return 0x80004001;}

// IEnumPins
struct EnumPins { void* v; LONG ref; Filter* f; int idx; };
static HRESULT __stdcall EP_QI(void*o,const GUID*i,void**out){EnumPins*e=(EnumPins*)o;if(geq(i,&IID_IUnknown_)||geq(i,&IID_IEnumPins_)){*out=o;InterlockedIncrement(&e->ref);return 0;}*out=0;return 0x80004002;}
static ULONG __stdcall EP_AR(void*o){return InterlockedIncrement(&((EnumPins*)o)->ref);}
static ULONG __stdcall EP_RL(void*o){EnumPins*e=(EnumPins*)o;LONG r=InterlockedDecrement(&e->ref);if(!r)LocalFree(e);return r;}
static HRESULT __stdcall EP_Next(void*o,ULONG c,void**out,ULONG*got){EnumPins*e=(EnumPins*)o;ULONG n=0;while(n<c&&e->idx<1){Pin*p=e->f->pin;out[n]=&p->v;InterlockedIncrement(&p->ref);n++;e->idx++;}if(got)*got=n;return n==c?0:1;}
static HRESULT __stdcall EP_Skip(void*o,ULONG c){((EnumPins*)o)->idx+=c;return 0;}
static HRESULT __stdcall EP_Reset(void*o){((EnumPins*)o)->idx=0;return 0;}
static HRESULT __stdcall EP_Clone(void*o,void**p){(void)o;*p=0;return 0x80004001;}

// IPin (output, push)
static HRESULT __stdcall P_QI(void*o,const GUID*i,void**out){Pin*p=(Pin*)o;if(geq(i,&IID_IUnknown_)||geq(i,&IID_IPin_)){*out=&p->v;InterlockedIncrement(&p->ref);return 0;}*out=0;return 0x80004002;}
static ULONG __stdcall P_AR(void*o){return InterlockedIncrement(&((Pin*)o)->ref);}
static ULONG __stdcall P_RL(void*o){return InterlockedDecrement(&((Pin*)o)->ref);}
static HRESULT __stdcall P_Connect(void*o,void*recv,const AM_MEDIA_TYPE*pmt){
    Pin*p=(Pin*)o; HRESULT hr; (void)pmt; g_hit_connect=1;
    L("Pin::Connect offering AAC type to downstream");
    hr=((HRESULT(__stdcall*)(void*,void*,const AM_MEDIA_TYPE*))VT(recv)[4])(recv,&p->v,&p->mt); // ReceiveConnection
    Lx("  ReceiveConnection",hr); if(hr) return hr;
    p->peer=recv;
    hr=((HRESULT(__stdcall*)(void*,const GUID*,void**))VT(recv)[0])(recv,&IID_IMemInputPin_,&p->meminput); // QI IMemInputPin
    Lx("  QI(IMemInputPin)",hr); if(hr||!p->meminput) return hr?hr:0x80004002;
    { void* a=NULL; ALLOC_PROPS props; ALLOC_PROPS act;
      hr=((HRESULT(__stdcall*)(void*,void**))VT(p->meminput)[3])(p->meminput,&a); Lx("  GetAllocator",hr);
      if(hr||!a){ L("  no allocator"); return hr?hr:0x80004005; }
      p->alloc=a;
      memset(&props,0,sizeof(props)); props.cBuffers=8; props.cbBuffer=16384; props.cbAlign=1; props.cbPrefix=0;
      hr=((HRESULT(__stdcall*)(void*,ALLOC_PROPS*,ALLOC_PROPS*))VT(a)[3])(a,&props,&act); Lx("  SetProperties",hr);
      hr=((HRESULT(__stdcall*)(void*,void*,int))VT(p->meminput)[4])(p->meminput,a,0); Lx("  NotifyAllocator",hr);
      hr=((HRESULT(__stdcall*)(void*))VT(a)[5])(a); Lx("  Commit",hr);
    }
    L("Pin::Connect OK");
    return 0;
}
static HRESULT __stdcall P_ReceiveConnection(void*o,void*c,const AM_MEDIA_TYPE*m){(void)o;(void)c;(void)m;return 0x80004001;}
static HRESULT __stdcall P_Disconnect(void*o){Pin*p=(Pin*)o;p->peer=0;return 0;}
static HRESULT __stdcall P_ConnectedTo(void*o,void**pp){Pin*p=(Pin*)o;if(!p->peer){*pp=0;return 0x80040209;}*pp=p->peer;((ULONG(__stdcall*)(void*))VT(p->peer)[1])(p->peer);return 0;}
static HRESULT __stdcall P_ConnectionMediaType(void*o,AM_MEDIA_TYPE*m){Pin*p=(Pin*)o;*m=p->mt;return 0;}
static HRESULT __stdcall P_QueryPinInfo(void*o,PIN_INFO*info){Pin*p=(Pin*)o;info->pFilter=&p->filter->vFilter;((ULONG(__stdcall*)(void*))g_FVtbl[1])(&p->filter->vFilter);info->dir=1;wcscpy(info->achName,L"Out");return 0;}
static HRESULT __stdcall P_QueryDirection(void*o,int*d){(void)o;*d=1;return 0;}
static HRESULT __stdcall P_QueryId(void*o,wchar_t**id){(void)o;*id=0;return 0x80004001;}
static HRESULT __stdcall P_QueryAccept(void*o,const AM_MEDIA_TYPE*m){(void)o;return geq(&m->majortype,&MT_major)?0:1;}
static HRESULT __stdcall P_EnumMediaTypes(void*o,void**e){(void)o;EnumMT*em=(EnumMT*)LocalAlloc(LPTR,sizeof(EnumMT));if(!em)return 0x8007000e;em->v=g_EMVtbl;em->ref=1;em->idx=0;*e=em;return 0;}
static HRESULT __stdcall P_QueryInternalConnections(void*o,void**a,ULONG*n){(void)o;(void)a;if(n)*n=0;return 0;}
static HRESULT __stdcall P_EndOfStream(void*o){(void)o;return 0;}
static HRESULT __stdcall P_BeginFlush(void*o){(void)o;return 0;}
static HRESULT __stdcall P_EndFlush(void*o){(void)o;return 0;}
static HRESULT __stdcall P_NewSegment(void*o,LONGLONG a,LONGLONG b,double r){(void)o;(void)a;(void)b;(void)r;return 0;}

// IFileSourceFilter: Load opens the local de-fragged file (3a backend)
static int parse_samples(const wchar_t* path, unsigned** sizes_out, unsigned* n_out, unsigned* base_out);
static HRESULT __stdcall FSF_QI(void*o,const GUID*i,void**out){ Filter* f=FSF_to_Filter(o); return F_QI(&f->vFilter,i,out); }
static ULONG __stdcall FSF_AR(void*o){ return InterlockedIncrement(&FSF_to_Filter(o)->ref); }
static ULONG __stdcall FSF_RL(void*o){ return InterlockedDecrement(&FSF_to_Filter(o)->ref); }
static HRESULT __stdcall FSF_Load(void*o,const wchar_t* name,const AM_MEDIA_TYPE* pmt){
    Filter* f=FSF_to_Filter(o); (void)pmt; g_hit_load=1;
    { char u[260]; int i=0; if(name){while(name[i]&&i<259){u[i]=(char)name[i];i++;}} u[i]=0;
      char b[320]; _snprintf(b,sizeof(b),">>> IFileSourceFilter::Load url=\"%s\"",u); L(b); }
    if(name) wcsncpy(f->curfile,name,259), f->curfile[259]=0;
    // ytm://NAME -> \flash2\automation\NAME.m4a  (3a local backend; 3b = ce_https)
    const wchar_t* id=name; if(name){ const wchar_t* s=wcsstr(name,L"://"); if(s) id=s+3; }
    wchar_t path[MAX_PATH]; _snwprintf(path,MAX_PATH-1,L"\\flash2\\automation\\%s.m4a",(id&&*id)?id:L"ytm-plain"); path[MAX_PATH-1]=0;
    { char b[260]; char pp[260]; int i=0; while(path[i]&&i<259){pp[i]=(char)path[i];i++;} pp[i]=0; _snprintf(b,sizeof(b),"  backend file = %s",pp); L(b); }
    if(!parse_samples(path,&f->sizes,&f->nsamp,&f->base)){ L("  Load: parse_samples failed"); return 0x80070002; }
    f->srcf=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(f->srcf==INVALID_HANDLE_VALUE){ L("  Load: open failed"); return 0x80070002; }
    { char b[120]; _snprintf(b,sizeof(b),"  Load ok: samples=%u base_off=%u",f->nsamp,f->base); L(b); }
    return 0;
}
static HRESULT __stdcall FSF_GetCurFile(void*o,wchar_t** ppName,AM_MEDIA_TYPE* pmt){ Filter* f=FSF_to_Filter(o); (void)pmt; if(ppName)*ppName=f->curfile; return 0; }

// IBaseFilter
static HRESULT __stdcall F_QI(void*o,const GUID*i,void**out){
    Filter*f=(Filter*)o;
    if(geq(i,&IID_IUnknown_)||geq(i,&IID_IPersist_)||geq(i,&IID_IMediaFilter_)||geq(i,&IID_IBaseFilter_)){*out=&f->vFilter;InterlockedIncrement(&f->ref);return 0;}
    if(geq(i,&IID_IFileSourceFilter_)){*out=&f->vFSF;InterlockedIncrement(&f->ref);L("F_QI(IFileSourceFilter)");return 0;}
    *out=0;return 0x80004002;
}
static ULONG __stdcall F_AR(void*o){return InterlockedIncrement(&((Filter*)o)->ref);}
static ULONG __stdcall F_RL(void*o){
    Filter* f=(Filter*)o; LONG r=InterlockedDecrement(&f->ref);
    if(r==0){ if(f->hThread){if(f->hStop)SetEvent(f->hStop);WaitForSingleObject(f->hThread,5000);CloseHandle(f->hThread);} if(f->srcf&&f->srcf!=INVALID_HANDLE_VALUE)CloseHandle(f->srcf); if(f->sizes)free(f->sizes); if(f->pin)LocalFree(f->pin); LocalFree(f); InterlockedDecrement((LONG*)&g_objcount); }
    return r;
}
static HRESULT __stdcall F_GetClassID(void*o,GUID*id){(void)o;*id=CLSID_OURS;return 0;}
static HRESULT __stdcall F_Stop(void*o){Filter*f=(Filter*)o;f->state=0;L("Filter::Stop");
    if(f->hThread){ if(f->hStop)SetEvent(f->hStop); WaitForSingleObject(f->hThread,5000); CloseHandle(f->hThread); f->hThread=0; } return 0;}
static HRESULT __stdcall F_Pause(void*o){Filter*f=(Filter*)o;f->state=1;L("Filter::Pause");
    if(!f->hThread && f->pin->meminput){ f->hStop=CreateEventW(NULL,TRUE,FALSE,NULL); f->hThread=CreateThread(NULL,0,push_worker,f,0,NULL); L("  started push worker"); } return 0;}
static int g_dumped=0;
static HRESULT __stdcall F_Run(void*o,LONGLONG t){(void)t; Filter* f=(Filter*)o; f->state=2; L("Filter::Run");
    if(!g_dumped && f->graph){ g_dumped=1; void* graph=f->graph; void* ef=NULL;
      if(((HRESULT(__stdcall*)(void*,void**))VT(graph)[5])(graph,&ef)==0&&ef){ void* fl=NULL; ULONG g=0; int c=0;
        while(((HRESULT(__stdcall*)(void*,ULONG,void**,ULONG*))VT(ef)[3])(ef,1,&fl,&g)==0&&g==1){
          FILTER_INFO fi; char nb[140]; ((HRESULT(__stdcall*)(void*,FILTER_INFO*))VT(fl)[12])(fl,&fi);
          {int i;for(i=0;fi.achName[i]&&i<120;i++)nb[i]=(char)fi.achName[i];nb[i]=0;}
          {char b[160];_snprintf(b,sizeof(b),"  zmedia graph[%d]: %s",c,nb);L(b);}
          if(fi.pGraph)((ULONG(__stdcall*)(void*))VT(fi.pGraph)[2])(fi.pGraph);
          ((ULONG(__stdcall*)(void*))VT(fl)[2])(fl); fl=0; c++;
        }
        ((ULONG(__stdcall*)(void*))VT(ef)[2])(ef);
      }
    }
    return 0;
}
static HRESULT __stdcall F_GetState(void*o,DWORD ms,int*st){(void)ms;*st=((Filter*)o)->state;return 0;}
static HRESULT __stdcall F_SetSyncSource(void*o,void*c){(void)o;(void)c;return 0;}
static HRESULT __stdcall F_GetSyncSource(void*o,void**c){(void)o;*c=0;return 0;}
static HRESULT __stdcall F_EnumPins(void*o,void**e){Filter*f=(Filter*)o;EnumPins*ep=(EnumPins*)LocalAlloc(LPTR,sizeof(EnumPins));if(!ep)return 0x8007000e;ep->v=g_EPVtbl;ep->ref=1;ep->f=f;ep->idx=0;*e=ep;L("Filter::EnumPins");return 0;}
static HRESULT __stdcall F_FindPin(void*o,const wchar_t*id,void**pp){(void)o;(void)id;*pp=0;return 0x80004005;}
static HRESULT __stdcall F_QueryFilterInfo(void*o,FILTER_INFO*info){Filter*f=(Filter*)o;wcscpy(info->achName,L"YtmSource");info->pGraph=f->graph;if(f->graph)((ULONG(__stdcall*)(void*))VT(f->graph)[1])(f->graph);return 0;}
static HRESULT __stdcall F_JoinFilterGraph(void*o,void*g,const wchar_t*n){(void)n;((Filter*)o)->graph=g;return 0;}
static HRESULT __stdcall F_QueryVendorInfo(void*o,wchar_t**v){(void)o;*v=0;return 0x80004001;}

static Filter* make_filter(){
    Filter* f=(Filter*)LocalAlloc(LPTR,sizeof(Filter));
    Pin* p=(Pin*)LocalAlloc(LPTR,sizeof(Pin));
    if(!f||!p){ if(f)LocalFree(f); if(p)LocalFree(p); return NULL; }
    f->vFilter=g_FVtbl; f->vFSF=g_FSFVtbl; f->ref=1; f->pin=p; f->graph=0; f->state=0;
    f->sizes=0; f->nsamp=0; f->base=0; f->srcf=INVALID_HANDLE_VALUE; f->hThread=0; f->hStop=0; f->curfile[0]=0;
    p->v=g_PVtbl; p->ref=1; p->filter=f; p->peer=0; p->meminput=0; p->alloc=0; fill_mt(&p->mt);
    InterlockedIncrement((LONG*)&g_objcount);
    return f;
}

// IClassFactory
struct ClassFactory { void* v; LONG ref; };
static ClassFactory g_factory;
static HRESULT __stdcall CF_QI(void*o,const GUID*i,void**out){if(geq(i,&IID_IUnknown_)||geq(i,&IID_IClassFactory_)){*out=o;return 0;}*out=0;return 0x80004002;}
static ULONG __stdcall CF_AR(void*o){return InterlockedIncrement(&((ClassFactory*)o)->ref);}
static ULONG __stdcall CF_RL(void*o){return InterlockedDecrement(&((ClassFactory*)o)->ref);}
static HRESULT __stdcall CF_CreateInstance(void*o,void*outer,const GUID*iid,void**ppv){
    (void)o;(void)outer; g_hit_createinst=1; L(">>> IClassFactory::CreateInstance");
    Filter* f=make_filter(); if(!f){*ppv=0;return 0x8007000e;}
    HRESULT hr=F_QI(&f->vFilter,iid,ppv); F_RL(&f->vFilter); return hr;
}
static HRESULT __stdcall CF_LockServer(void*o,BOOL f){(void)o;(void)f;return 0;}

static void init_vtbls(){
    void* F[15]={(void*)F_QI,(void*)F_AR,(void*)F_RL,(void*)F_GetClassID,(void*)F_Stop,(void*)F_Pause,(void*)F_Run,(void*)F_GetState,(void*)F_SetSyncSource,(void*)F_GetSyncSource,(void*)F_EnumPins,(void*)F_FindPin,(void*)F_QueryFilterInfo,(void*)F_JoinFilterGraph,(void*)F_QueryVendorInfo};
    void* S[5]={(void*)FSF_QI,(void*)FSF_AR,(void*)FSF_RL,(void*)FSF_Load,(void*)FSF_GetCurFile};
    void* P[18]={(void*)P_QI,(void*)P_AR,(void*)P_RL,(void*)P_Connect,(void*)P_ReceiveConnection,(void*)P_Disconnect,(void*)P_ConnectedTo,(void*)P_ConnectionMediaType,(void*)P_QueryPinInfo,(void*)P_QueryDirection,(void*)P_QueryId,(void*)P_QueryAccept,(void*)P_EnumMediaTypes,(void*)P_QueryInternalConnections,(void*)P_EndOfStream,(void*)P_BeginFlush,(void*)P_EndFlush,(void*)P_NewSegment};
    void* EP[7]={(void*)EP_QI,(void*)EP_AR,(void*)EP_RL,(void*)EP_Next,(void*)EP_Skip,(void*)EP_Reset,(void*)EP_Clone};
    void* EM[7]={(void*)EM_QI,(void*)EM_AR,(void*)EM_RL,(void*)EM_Next,(void*)EM_Skip,(void*)EM_Reset,(void*)EM_Clone};
    void* CF[5]={(void*)CF_QI,(void*)CF_AR,(void*)CF_RL,(void*)CF_CreateInstance,(void*)CF_LockServer};
    memcpy(g_FVtbl,F,sizeof(F));memcpy(g_FSFVtbl,S,sizeof(S));memcpy(g_PVtbl,P,sizeof(P));memcpy(g_EPVtbl,EP,sizeof(EP));memcpy(g_EMVtbl,EM,sizeof(EM));memcpy(g_CFVtbl,CF,sizeof(CF));
    g_factory.v=g_CFVtbl; g_factory.ref=1;
}

// ── parse local plain MP4 sample table (stsz sizes + stco base offset) ──────
static int parse_samples(const wchar_t* path, unsigned** sizes_out, unsigned* n_out, unsigned* base_out){
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    DWORD sz,got=0; unsigned char* d;
    if(h==INVALID_HANDLE_VALUE) return 0;
    sz=GetFileSize(h,NULL); d=(unsigned char*)malloc(sz<262144?262144:sz);
    if(!d){CloseHandle(h);return 0;}
    ReadFile(h,d,sz,&got,NULL); CloseHandle(h);
    unsigned i; unsigned stsz=0, stco=0;
    for(i=0;i+8<got;i++){ if(d[i+4]=='s'&&d[i+5]=='t'&&d[i+6]=='s'&&d[i+7]=='z') stsz=i;
                          if(d[i+4]=='s'&&d[i+5]=='t'&&d[i+6]=='c'&&d[i+7]=='o') stco=i; }
    if(!stsz||!stco){ free(d); return 0; }
    { unsigned cnt=rd32(d+stsz+8+8); unsigned* arr=(unsigned*)malloc(cnt*sizeof(unsigned)); unsigned k;
      if(!arr){free(d);return 0;}
      for(k=0;k<cnt;k++) arr[k]=rd32(d+stsz+8+12+k*4);
      *sizes_out=arr; *n_out=cnt; *base_out=rd32(d+stco+8+8); }
    free(d); return 1;
}

// push worker started on Filter::Pause: preroll then clock-paced Receive
static DWORD WINAPI push_worker(LPVOID param){
    Filter* f=(Filter*)param; Pin* p=f->pin; char line[160];
    unsigned k; unsigned long off=f->base; LONGLONG t=0; const LONGLONG dur=232199; int pushed=0;
    DWORD t_start=GetTickCount();
    L("push_worker: begin");
    for(k=0;k<f->nsamp;k++){
        void* samp=NULL; unsigned char* ptr=NULL; DWORD rd=0; LONGLONG t0=t,t1=t+dur; HRESULT hr;
        if(WaitForSingleObject(f->hStop,0)==WAIT_OBJECT_0){ L("push_worker: stop"); break; }
        { DWORD target=t_start+(DWORD)(((unsigned long long)k*1024*1000)/44100); DWORD now=GetTickCount();
          if((long)(target-now)>2) WaitForSingleObject(f->hStop,(target-now)); }
        hr=((HRESULT(__stdcall*)(void*,void**,LONGLONG*,LONGLONG*,DWORD))VT(p->alloc)[7])(p->alloc,&samp,NULL,NULL,0); // GetBuffer
        if(hr||!samp){ Lx("push GetBuffer",hr); break; }
        ((HRESULT(__stdcall*)(void*,unsigned char**))VT(samp)[3])(samp,&ptr);
        SetFilePointer(f->srcf,(LONG)off,NULL,FILE_BEGIN); ReadFile(f->srcf,ptr,f->sizes[k],&rd,NULL);
        ((HRESULT(__stdcall*)(void*,unsigned long))VT(samp)[12])(samp,(unsigned long)f->sizes[k]);
        (void)t0;(void)t1;
        ((HRESULT(__stdcall*)(void*,LONGLONG*,LONGLONG*))VT(samp)[6])(samp,NULL,NULL); // untimed: render as received
        ((HRESULT(__stdcall*)(void*,int))VT(samp)[8])(samp,1);
        if(k==0) ((HRESULT(__stdcall*)(void*,int))VT(samp)[16])(samp,1);
        hr=((HRESULT(__stdcall*)(void*,void*))VT(p->meminput)[6])(p->meminput,samp); // Receive
        if(k<3){ char b[96]; _snprintf(b,sizeof(b),"  Receive[%u] hr=0x%08x size=%lu",k,hr,(unsigned long)f->sizes[k]); L(b); }
        ((ULONG(__stdcall*)(void*))VT(samp)[2])(samp);
        if(hr){ Lx("push Receive",hr); break; }
        off+=f->sizes[k]; t+=dur; pushed++;
        if((k%400)==0){ _snprintf(line,sizeof(line),"  pushed %d/%u t=%ldms",pushed,f->nsamp,(long)(t/10000)); L(line); }
    }
    _snprintf(line,sizeof(line),"push_worker done pushed=%d/%u",pushed,f->nsamp); L(line);
    if(p->peer) ((HRESULT(__stdcall*)(void*))VT(p->peer)[14])(p->peer); // EndOfStream
    return 0;
}

// ── COM server exports (via .def) ────────────────────────────────────────────
STDAPI DllGetClassObject(REFCLSID rclsid,REFIID riid,void** ppv){
    g_hit_dllgco=1; L(">>> DllGetClassObject fired");
    if(geq(&rclsid,&CLSID_OURS) && (geq(&riid,&IID_IClassFactory_)||geq(&riid,&IID_IUnknown_))){ *ppv=&g_factory; return 0; }
    *ppv=0; return 0x80040111;
}
STDAPI DllCanUnloadNow(void){ return g_objcount>0 ? 1 : 0; } /* unload when idle -> redeploy without reboot */

// ── registry (HKCR, RAM hive) ────────────────────────────────────────────────
static void reg_setup(){
    HKEY k; DWORD disp; DWORD merit=0x800002;
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,TEST_SCHEME,0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,L"Source Filter",0,REG_SZ,(BYTE*)CLSID_STR,(DWORD)((wcslen(CLSID_STR)+1)*2));
        RegSetValueExW(k,L"URL Protocol",0,REG_SZ,(BYTE*)L"",2); RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR,0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)L"Xune YouTube Source",(DWORD)((wcslen(L"Xune YouTube Source")+1)*2));
        RegSetValueExW(k,L"Merit",0,REG_DWORD,(BYTE*)&merit,4); RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR L"\\InprocServer32",0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)DLL_PATH,(DWORD)((wcslen(DLL_PATH)+1)*2));
        RegSetValueExW(k,L"ThreadingModel",0,REG_SZ,(BYTE*)L"Both",10); RegCloseKey(k);
    }
}
static void reg_teardown(){
    RegDeleteKeyW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR L"\\InprocServer32");
    RegDeleteKeyW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR);
    RegDeleteKeyW(HKEY_CLASSES_ROOT,TEST_SCHEME);
}

// RunDaemon (nativeapp plugin entry, spawn/stop): register the ytm:// scheme,
// then play it through the REAL native queue via ZDKMedia_Queue_PlaySongFromURL.
// zmedia_serv builds its native graph, CoCreates OUR source via the scheme
// (loaded from flash by InprocServer32), connects it to the native AAC decoder
// + renderer, and owns the real audio output + transport. We poll the queue.
extern "C" __declspec(dllexport) int RunDaemon(const void *arg, int arg_len, HANDLE stop_event){
    typedef int (*play_url_fn)(const wchar_t*, const wchar_t*);
    typedef int (*get_int_fn)(int*);
    char line[200]; HMODULE zdk; int hr, i;
    play_url_fn play; get_int_fn getpos, getcount, getidx;
    (void)arg;(void)arg_len;

    L("=== ce_dshow_ytmsrc: register ytm:// scheme -> ZDKMedia_Queue_PlaySongFromURL ===");
    init_vtbls(); // also done in DllMain; idempotent

    reg_setup();
    L("registry written: HKCR\\ytm\\Source Filter + HKCR\\CLSID\\{ours}\\InprocServer32 -> our dll");

    zdk=LoadLibraryW(L"zdksystem.dll");
    if(!zdk){ L("LoadLibrary(zdksystem) failed"); return -2; }
    play    =(play_url_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_PlaySongFromURL");
    getpos  =(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetPlayPosition");
    getcount=(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetSongCount");
    getidx  =(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetActiveSongIndex");
    if(!play){ L("ZDKMedia_Queue_PlaySongFromURL unresolved"); return -3; }

    L("calling ZDKMedia_Queue_PlaySongFromURL(L\"YTM Test\", L\"ytm://ytm-plain\") ...");
    hr=play(L"YTM Test", TEST_URL);
    _snprintf(line,sizeof(line),"PlaySongFromURL hr=0x%08x (0=ok)",hr); L(line);

    for(i=0;i<90;i++){
        int pos=-1,cnt=-1,idx=-1,hp,hc,hi;
        if(WaitForSingleObject(stop_event,2000)==WAIT_OBJECT_0){ L("stop signalled"); break; }
        hp=getpos?getpos(&pos):-1; hc=getcount?getcount(&cnt):-1; hi=getidx?getidx(&idx):-1;
        _snprintf(line,sizeof(line),"  t+%ds  pos hr=0x%08x pos_ms=%d | count(hr=0x%08x)=%d idx(hr=0x%08x)=%d",
                  (i+1)*2, hp, pos, hc, cnt, hi, idx); L(line);
    }
    // leave registry in place so zmedia keeps resolving ytm:// across the session
    L("--- exit (scheme left registered)");
    return 0;
}
// RunFile, an isolation test: play ytm-plain.m4a via the NATIVE PlaySongFromFile
// (native MP4 source, not ours). If audible, the file+decoder+renderer chain is
// good and any RunDaemon silence is our push source's fault.
extern "C" __declspec(dllexport) int RunFile(const void *arg, int arg_len, HANDLE stop_event){
    typedef int (*play_file_fn)(const wchar_t*, const wchar_t*, int);
    typedef int (*get_int_fn)(int*);
    char line[200]; HMODULE zdk; int hr, i; play_file_fn play; get_int_fn getpos;
    (void)arg;(void)arg_len;
    L("=== ce_dshow_ytmsrc RunFile: NATIVE PlaySongFromFile(ytm-plain.m4a) ===");
    zdk=LoadLibraryW(L"zdksystem.dll"); if(!zdk){L("no zdksystem");return -2;}
    play=(play_file_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_PlaySongFromFile");
    getpos=(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetPlayPosition");
    if(!play){L("PlaySongFromFile unresolved");return -3;}
    hr=play(L"YTM File Test", L"\\flash2\\automation\\ytm-plain.m4a", 213000);
    _snprintf(line,sizeof(line),"PlaySongFromFile hr=0x%08x",hr); L(line);
    for(i=0;i<30;i++){ int pos=-1; if(WaitForSingleObject(stop_event,2000)==WAIT_OBJECT_0)break; if(getpos)getpos(&pos); _snprintf(line,sizeof(line),"  t+%ds pos_ms=%d",(i+1)*2,pos); L(line); }
    L("--- exit"); return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h,DWORD r,LPVOID l){(void)h;(void)l; if(r==DLL_PROCESS_ATTACH) init_vtbls(); return TRUE;}
