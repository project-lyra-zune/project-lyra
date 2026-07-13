// ce_dshow_ytmstream_plugin  (daemon entry: RunDaemon)
//
// directshow-stream milestone 3b: the ytm:// source with a STREAMING backend.
// Same registered-source mechanism + media-type contract as 3a
// (ce_dshow_ytmsrc_plugin), but Load fetches YouTube-Music audio over the
// network and de-frags it on the fly instead of reading a local file:
//
//   IFileSourceFilter::Load("ytm://<videoId>")
//     -> ANDROID_VR /player -> itag-140 url
//     -> ce_https keep-alive conn; phase-1 Range-walk the moofs -> incremental
//        de-frag -> finish gives the moov (parse stsz = per-AU sizes) + total mdat
//     -> malloc(total_mdat); start fetch thread streaming fragment mdat payloads
//        into the buffer (advancing g_avail)
//   push_worker (on Filter::Pause): push AU k once g_avail covers it; the native
//     renderer clock-paces, so the push naturally tracks the ~realtime download.
//
// The push-source model sidesteps PlaySongFromFile's load-at-open limit: the
// decoder pulls live, our worker blocks on the available-bytes counter. Output
// media type: major MEDIATYPE_Audio + subtype {0000160A} + WAVEFORMATEX{2ch,44100}
// (the Receive-path AAC type that configures the NvMM block from the format).

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "ce_https.h"
#include "ce_innertube.h"
#include "ce_mp4_defrag.h"

#define DLL_PATH    L"\\flash2\\automation\\ce_ytmstream.dll"
#define TEST_SCHEME L"ytm"
#define CLSID_STR   L"{C0FFEE06-2026-4A6E-B0DE-5965594A5959}"
#define TEST_URL    L"ytm://dQw4w9WgXcQ"

// itag-140 googlevideo URLs are final (no redirect); this UA rides every ranged GET.
#define VR_HDRS "User-Agent: com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12; Quest 3) gzip\r\n" \
                "X-YouTube-Client-Name: 28\r\nX-YouTube-Client-Version: 1.60.19"
#define FRONT_WIN   4096
#define MOOF_WIN    3072
#define STREAM_WIN  262144
#define MAX_FRAGS   256
#define PREBUFFER_BYTES 65536

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
static const GUID MT_major  = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}}; // MEDIATYPE_Audio
static const GUID MT_sub    = {0x0000160a,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}}; // AAC (Receive path)
static const GUID FORMAT_WaveFormatEx_ = {0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static const GUID GUID_NULL_ = {0,0,0,{0,0,0,0,0,0,0,0}};
static const GUID CLSID_OURS = {0xC0FFEE06,0x2026,0x4A6E,{0xB0,0xDE,0x59,0x65,0x59,0x4A,0x59,0x59}};
static const GUID CLSID_FilterGraph = {0xe436ebb3,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IGraphBuilder = {0x56a868a9,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID IID_IMediaControl = {0x56a868b1,0x0ad4,0x11ce,{0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

struct AM_MEDIA_TYPE { GUID majortype, subtype; int bFixed, bTemporal; unsigned long lSampleSize; GUID formattype; void* pUnk; unsigned long cbFormat; unsigned char* pbFormat; };
struct PIN_INFO { void* pFilter; int dir; wchar_t achName[128]; };
struct FILTER_INFO { wchar_t achName[128]; void* pGraph; };
struct ALLOC_PROPS { long cBuffers, cbBuffer, cbAlign, cbPrefix; };

static int g_objcount=0;
static void* (__stdcall *g_CoTaskMemAlloc)(SIZE_T);
static unsigned char g_wfx[18] = {0x0a,0x16, 2,0, 0x44,0xac,0,0, 0,0,0,0, 0,0, 0,0, 0,0}; // tag0x160a,2ch,44100,cbSize0

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

struct Pin { void* v; LONG ref; struct Filter* filter; void* peer; void* meminput; void* alloc; AM_MEDIA_TYPE mt; };
struct Filter {
    void* vFilter; void* vFSF;
    LONG ref; Pin* pin; void* graph; int state;
    unsigned* sizes; unsigned nsamp;
    unsigned char* mdat; unsigned long total_mdat;
    volatile LONG avail; volatile LONG fetch_err; volatile LONG fetch_done;
    ce_https_conn* conn;
    unsigned long mdat_off[MAX_FRAGS], mdat_len[MAX_FRAGS]; int nfrag;
    HANDLE hThread, hStop, hFetch;
    wchar_t curfile[260];
};
static Filter* FSF_to_Filter(void* o){ return (Filter*)((char*)o - sizeof(void*)); }
static void* g_FVtbl[15]; static void* g_FSFVtbl[5]; static void* g_PVtbl[18]; static void* g_EPVtbl[7]; static void* g_EMVtbl[7]; static void* g_CFVtbl[5];
static DWORD WINAPI push_worker(LPVOID param);
static DWORD WINAPI fetch_thread(LPVOID param);
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
    Pin*p=(Pin*)o; HRESULT hr; (void)pmt;
    L("Pin::Connect offering AAC type to downstream");
    hr=((HRESULT(__stdcall*)(void*,void*,const AM_MEDIA_TYPE*))VT(recv)[4])(recv,&p->v,&p->mt);
    Lx("  ReceiveConnection",hr); if(hr) return hr;
    p->peer=recv;
    hr=((HRESULT(__stdcall*)(void*,const GUID*,void**))VT(recv)[0])(recv,&IID_IMemInputPin_,&p->meminput);
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

// parse stsz sample sizes from a buffer holding the de-fragged moov (prefix)
static int parse_stsz_buf(const unsigned char* d, size_t got, unsigned** sizes_out, unsigned* n_out){
    size_t i; size_t stsz=0;
    for(i=0;i+8<got;i++){ if(d[i+4]=='s'&&d[i+5]=='t'&&d[i+6]=='s'&&d[i+7]=='z') stsz=i; }
    if(!stsz) return 0;
    { unsigned cnt=rd32(d+stsz+8+8); unsigned* arr; unsigned k;
      if(!cnt||cnt>200000) return 0;
      arr=(unsigned*)malloc(cnt*sizeof(unsigned)); if(!arr) return 0;
      for(k=0;k<cnt;k++) arr[k]=rd32(d+stsz+8+12+k*4);
      *sizes_out=arr; *n_out=cnt; }
    return 1;
}

// IFileSourceFilter: Load streams ytm://<videoId> (3b backend)
static HRESULT __stdcall FSF_QI(void*o,const GUID*i,void**out){ Filter* f=FSF_to_Filter(o); return F_QI(&f->vFilter,i,out); }
static ULONG __stdcall FSF_AR(void*o){ return InterlockedIncrement(&FSF_to_Filter(o)->ref); }
static ULONG __stdcall FSF_RL(void*o){ return InterlockedDecrement(&FSF_to_Filter(o)->ref); }
static HRESULT __stdcall FSF_Load(void*o,const wchar_t* name,const AM_MEDIA_TYPE* pmt){
    Filter* f=FSF_to_Filter(o); (void)pmt;
    char vid[64]; int vn=0;
    enum ce_https_result r; char* url=NULL;
    unsigned char* front=NULL; unsigned char* prefix=NULL; ce_mp4_defrag_ctx* ctx=NULL;
    HRESULT result=0x80004005;

    if(name) wcsncpy(f->curfile,name,259), f->curfile[259]=0;
    { const wchar_t* id=name; if(name){ const wchar_t* s=wcsstr(name,L"://"); if(s) id=s+3; }
      while(id && *id && vn<63){ vid[vn++]=(char)*id++; } vid[vn]=0; }
    { char b[120]; _snprintf(b,sizeof(b),">>> Load stream videoId=\"%s\"",vid); L(b); }
    if(vn<5){ L("  bad videoId"); return 0x80070057; }

    { WSADATA w; WSAStartup(MAKEWORD(2,2),&w); }  // no WSACleanup (daemon-plugin hang pitfall)

    url=(char*)malloc(2048);
    if(!url){ L("  oom"); return result; }
    { enum ce_innertube_result it=ce_innertube_audio_url(vid,url,2048);
      if(it!=CE_IT_OK){ Lx("  audio_url failed",(HRESULT)it); free(url); return result; } }
    L("  got itag-140 url");

    f->conn=ce_https_conn_open(url, VR_HDRS, &r); free(url); url=NULL;
    if(!f->conn){ Lx("  conn_open failed",(HRESULT)r); return result; }

    // phase 1: walk moofs, build the moov, record fragment mdat offsets/lengths
    front=(unsigned char*)malloc(FRONT_WIN);
    unsigned char* win=(unsigned char*)malloc(STREAM_WIN);
    if(!front||!win){ L("  oom"); goto done; }
    {
        size_t got=0; unsigned long total=0, off, moov_end=0, first_moof=0; size_t s=0;
        r=ce_https_conn_get(f->conn,0,FRONT_WIN,front,FRONT_WIN,&got,&total,NULL);
        if(r!=CE_HTTPS_OK){ Lx("  front get failed",(HRESULT)r); goto done; }
        while(s+8<=got){ unsigned sz=rd32(front+s); const unsigned char* t=front+s+4;
            if(sz<8) break;
            if(!memcmp(t,"moov",4)) moov_end=(unsigned long)(s+sz);
            if(!memcmp(t,"moof",4)){ first_moof=(unsigned long)s; break; }
            s+=sz; }
        if(!moov_end||!first_moof){ L("  no moov/moof in front"); goto done; }
        ctx=ce_mp4_defrag_begin(front, moov_end);
        if(!ctx){ L("  defrag_begin failed"); goto done; }
        off=first_moof;
        while(off<total && f->nfrag<MAX_FRAGS){
            unsigned long want=MOOF_WIN; size_t wl=0; unsigned sz; const unsigned char* t; const unsigned char* bp;
            if(want>total-off) want=total-off;
            if(off+want<=got){ bp=front+off; wl=want; }
            else { r=ce_https_conn_get(f->conn,off,want,win,STREAM_WIN,&wl,NULL,NULL);
                   if(r!=CE_HTTPS_OK){ Lx("  moof get failed",(HRESULT)r); goto done; } bp=win; }
            sz=rd32(bp); t=bp+4;
            if(memcmp(t,"moof",4)){ L("  expected moof"); goto done; }
            // moof+mdat-header bigger than the fast window (long-fragment trun); refetch exactly sz+8
            if((unsigned long)sz+8>wl){
                if((unsigned long)sz+8>STREAM_WIN){ L("  moof too large"); goto done; }
                r=ce_https_conn_get(f->conn,off,(unsigned long)sz+8,win,STREAM_WIN,&wl,NULL,NULL);
                if(r!=CE_HTTPS_OK){ Lx("  moof refetch failed",(HRESULT)r); goto done; }
                bp=win;
                if((unsigned long)sz+8>wl){ L("  moof short after refetch"); goto done; }
            }
            if(ce_mp4_defrag_feed_moof(ctx,bp,sz)!=CE_MP4_OK){ L("  feed_moof failed"); goto done; }
            { unsigned msz=rd32(bp+sz); const unsigned char* mt=bp+sz+4;
              if(memcmp(mt,"mdat",4)){ L("  expected mdat"); goto done; }
              f->mdat_off[f->nfrag]=off+sz+8; f->mdat_len[f->nfrag]=msz-8;
              ce_mp4_defrag_add_mdat(ctx,msz-8); off+=sz+msz; f->nfrag++; }
        }
    }
    {
        size_t prefix_len=0; unsigned long tm=0;
        if(ce_mp4_defrag_finish(ctx,&prefix,&prefix_len,&tm)!=CE_MP4_OK){ L("  finish failed"); goto done; }
        f->total_mdat=tm;
        if(!parse_stsz_buf(prefix,prefix_len,&f->sizes,&f->nsamp)){ L("  parse stsz failed"); goto done; }
        f->mdat=(unsigned char*)malloc(tm?tm:1);
        if(!f->mdat){ L("  mdat alloc failed"); goto done; }
        { char b[160]; _snprintf(b,sizeof(b),"  Load ok: frags=%d samples=%u total_mdat=%lu",f->nfrag,f->nsamp,tm); L(b); }
        f->hFetch=CreateThread(NULL,0,fetch_thread,f,0,NULL);
        result=0; // S_OK
    }
done:
    if(prefix) free(prefix);
    if(ctx) ce_mp4_defrag_free(ctx);
    if(front) free(front);
    if(win) free(win);
    if(result) { if(f->conn){ ce_https_conn_close(f->conn); f->conn=0; } }
    return result;
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
static void filter_stop_threads(Filter* f){
    if(f->hStop)SetEvent(f->hStop);
    if(f->hThread){WaitForSingleObject(f->hThread,5000);CloseHandle(f->hThread);f->hThread=0;}
    if(f->hFetch){WaitForSingleObject(f->hFetch,5000);CloseHandle(f->hFetch);f->hFetch=0;}
}
static ULONG __stdcall F_RL(void*o){
    Filter* f=(Filter*)o; LONG r=InterlockedDecrement(&f->ref);
    if(r==0){ filter_stop_threads(f); if(f->conn)ce_https_conn_close(f->conn);
        if(f->mdat)free(f->mdat); if(f->sizes)free(f->sizes); if(f->pin)LocalFree(f->pin); LocalFree(f);
        InterlockedDecrement((LONG*)&g_objcount); }
    return r;
}
static HRESULT __stdcall F_GetClassID(void*o,GUID*id){(void)o;*id=CLSID_OURS;return 0;}
static HRESULT __stdcall F_Stop(void*o){Filter*f=(Filter*)o;f->state=0;L("Filter::Stop"); filter_stop_threads(f); return 0;}
static HRESULT __stdcall F_Pause(void*o){Filter*f=(Filter*)o;f->state=1;L("Filter::Pause");
    if(!f->hThread && f->pin->meminput){ f->hThread=CreateThread(NULL,0,push_worker,f,0,NULL); L("  started push worker"); } return 0;}
static int g_dumped=0;
static HRESULT __stdcall F_Run(void*o,LONGLONG t){(void)t; Filter* f=(Filter*)o; f->state=2; L("Filter::Run");
    if(!g_dumped && f->graph){ g_dumped=1; void* graph=f->graph; void* ef=NULL;
      if(((HRESULT(__stdcall*)(void*,void**))VT(graph)[5])(graph,&ef)==0&&ef){ void* fl=NULL; ULONG g=0; int c=0;
        while(((HRESULT(__stdcall*)(void*,ULONG,void**,ULONG*))VT(ef)[3])(ef,1,&fl,&g)==0&&g==1){
          FILTER_INFO fi; char nb[140]; ((HRESULT(__stdcall*)(void*,FILTER_INFO*))VT(fl)[12])(fl,&fi);
          {int i;for(i=0;fi.achName[i]&&i<120;i++)nb[i]=(char)fi.achName[i];nb[i]=0;}
          {char b[160];_snprintf(b,sizeof(b),"  zmedia graph[%d]: %s",c,nb);L(b);}
          if(fi.pGraph)((ULONG(__stdcall*)(void*))VT(fi.pGraph)[2])(fi.pGraph);
          ((ULONG(__stdcall*)(void*))VT(fl)[2])(fl); fl=0; c++; }
        ((ULONG(__stdcall*)(void*))VT(ef)[2])(ef); } }
    return 0;
}
static HRESULT __stdcall F_GetState(void*o,DWORD ms,int*st){(void)ms;*st=((Filter*)o)->state;return 0;}
static HRESULT __stdcall F_SetSyncSource(void*o,void*c){(void)o;(void)c;return 0;}
static HRESULT __stdcall F_GetSyncSource(void*o,void**c){(void)o;*c=0;return 0;}
static HRESULT __stdcall F_EnumPins(void*o,void**e){Filter*f=(Filter*)o;EnumPins*ep=(EnumPins*)LocalAlloc(LPTR,sizeof(EnumPins));if(!ep)return 0x8007000e;ep->v=g_EPVtbl;ep->ref=1;ep->f=f;ep->idx=0;*e=ep;L("Filter::EnumPins");return 0;}
static HRESULT __stdcall F_FindPin(void*o,const wchar_t*id,void**pp){(void)o;(void)id;*pp=0;return 0x80004005;}
static HRESULT __stdcall F_QueryFilterInfo(void*o,FILTER_INFO*info){Filter*f=(Filter*)o;wcscpy(info->achName,L"YtmStream");info->pGraph=f->graph;if(f->graph)((ULONG(__stdcall*)(void*))VT(f->graph)[1])(f->graph);return 0;}
static HRESULT __stdcall F_JoinFilterGraph(void*o,void*g,const wchar_t*n){(void)n;((Filter*)o)->graph=g;return 0;}
static HRESULT __stdcall F_QueryVendorInfo(void*o,wchar_t**v){(void)o;*v=0;return 0x80004001;}

static Filter* make_filter(){
    Filter* f=(Filter*)LocalAlloc(LPTR,sizeof(Filter));
    Pin* p=(Pin*)LocalAlloc(LPTR,sizeof(Pin));
    if(!f||!p){ if(f)LocalFree(f); if(p)LocalFree(p); return NULL; }
    f->vFilter=g_FVtbl; f->vFSF=g_FSFVtbl; f->ref=1; f->pin=p; f->graph=0; f->state=0;
    f->sizes=0; f->nsamp=0; f->mdat=0; f->total_mdat=0; f->avail=0; f->fetch_err=0; f->fetch_done=0;
    f->conn=0; f->nfrag=0; f->hThread=0; f->hFetch=0; f->curfile[0]=0;
    f->hStop=CreateEventW(NULL,TRUE,FALSE,NULL);
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
    (void)o;(void)outer; L(">>> IClassFactory::CreateInstance");
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

// fetch thread: stream fragment mdat payloads into f->mdat, advancing f->avail
static DWORD WINAPI fetch_thread(LPVOID param){
    Filter* f=(Filter*)param; unsigned long wpos=0; int fr;
    unsigned char* tmp=(unsigned char*)malloc(STREAM_WIN);
    if(!tmp){ f->fetch_err=1; f->fetch_done=1; return 0; }
    L("fetch_thread: begin");
    for(fr=0; fr<f->nfrag; fr++){
        unsigned long done=0;
        while(done<f->mdat_len[fr]){
            unsigned long want=STREAM_WIN; size_t wl=0; enum ce_https_result r;
            if(WaitForSingleObject(f->hStop,0)==WAIT_OBJECT_0){ L("fetch_thread: stop"); goto end; }
            if(want>f->mdat_len[fr]-done) want=f->mdat_len[fr]-done;
            if(wpos+want>f->total_mdat) want=f->total_mdat-wpos;
            if(!want) goto end;
            r=ce_https_conn_get(f->conn,f->mdat_off[fr]+done,want,f->mdat+wpos,want,&wl,NULL,NULL);
            if(r!=CE_HTTPS_OK){ Lx("fetch get failed",(HRESULT)r); f->fetch_err=1; goto end; }
            done+=(unsigned long)wl; wpos+=(unsigned long)wl;
            InterlockedExchange(&f->avail,(LONG)wpos);
        }
    }
end:
    InterlockedExchange(&f->avail,(LONG)wpos);
    f->fetch_done=1;
    { char b[80]; _snprintf(b,sizeof(b),"fetch_thread done avail=%lu/%lu",wpos,f->total_mdat); L(b); }
    if(tmp) free(tmp);
    return 0;
}

// push worker started on Filter::Pause: pull AUs from the streamed buffer
static DWORD WINAPI push_worker(LPVOID param){
    Filter* f=(Filter*)param; Pin* p=f->pin; char line[160];
    unsigned k; unsigned long off=0; int pushed=0;
    L("push_worker: begin (streaming)");
    // small prebuffer so the renderer doesn't underrun immediately
    while((unsigned long)f->avail<PREBUFFER_BYTES && !f->fetch_done && !f->fetch_err){
        if(WaitForSingleObject(f->hStop,30)==WAIT_OBJECT_0){ L("push_worker: stop (prebuffer)"); return 0; }
    }
    { char b[64]; _snprintf(b,sizeof(b),"  prebuffered avail=%ld",f->avail); L(b); }
    for(k=0;k<f->nsamp;k++){
        void* samp=NULL; unsigned char* ptr=NULL; HRESULT hr; unsigned long need=off+f->sizes[k];
        if(WaitForSingleObject(f->hStop,0)==WAIT_OBJECT_0){ L("push_worker: stop"); break; }
        // wait until this AU's bytes are downloaded
        while((unsigned long)f->avail<need){
            if(f->fetch_err){ L("push_worker: fetch error"); goto eos; }
            if(f->fetch_done && (unsigned long)f->avail<need){ L("push_worker: short stream"); goto eos; }
            if(WaitForSingleObject(f->hStop,20)==WAIT_OBJECT_0){ L("push_worker: stop (wait)"); goto eos; }
        }
        hr=((HRESULT(__stdcall*)(void*,void**,LONGLONG*,LONGLONG*,DWORD))VT(p->alloc)[7])(p->alloc,&samp,NULL,NULL,0); // GetBuffer (clock-paces)
        if(hr||!samp){ Lx("push GetBuffer",hr); break; }
        ((HRESULT(__stdcall*)(void*,unsigned char**))VT(samp)[3])(samp,&ptr);
        memcpy(ptr, f->mdat+off, f->sizes[k]);
        ((HRESULT(__stdcall*)(void*,unsigned long))VT(samp)[12])(samp,(unsigned long)f->sizes[k]);
        ((HRESULT(__stdcall*)(void*,LONGLONG*,LONGLONG*))VT(samp)[6])(samp,NULL,NULL); // untimed
        ((HRESULT(__stdcall*)(void*,int))VT(samp)[8])(samp,1);
        if(k==0) ((HRESULT(__stdcall*)(void*,int))VT(samp)[16])(samp,1);
        hr=((HRESULT(__stdcall*)(void*,void*))VT(p->meminput)[6])(p->meminput,samp);
        if(k<3){ char b[96]; _snprintf(b,sizeof(b),"  Receive[%u] hr=0x%08x size=%lu",k,hr,(unsigned long)f->sizes[k]); L(b); }
        ((ULONG(__stdcall*)(void*))VT(samp)[2])(samp);
        if(hr){ Lx("push Receive",hr); break; }
        off+=f->sizes[k]; pushed++;
        if((k%400)==0){ _snprintf(line,sizeof(line),"  pushed %d/%u off=%lu avail=%ld",pushed,f->nsamp,off,f->avail); L(line); }
    }
eos:
    _snprintf(line,sizeof(line),"push_worker done pushed=%d/%u",pushed,f->nsamp); L(line);
    if(p->peer) ((HRESULT(__stdcall*)(void*))VT(p->peer)[14])(p->peer); // EndOfStream
    return 0;
}

// ── COM server exports (via .def) ────────────────────────────────────────────
STDAPI DllGetClassObject(REFCLSID rclsid,REFIID riid,void** ppv){
    L(">>> DllGetClassObject fired");
    if(geq(&rclsid,&CLSID_OURS) && (geq(&riid,&IID_IClassFactory_)||geq(&riid,&IID_IUnknown_))){ *ppv=&g_factory; return 0; }
    *ppv=0; return 0x80040111;
}
STDAPI DllCanUnloadNow(void){ return g_objcount>0 ? 1 : 0; }

static void reg_setup(){
    HKEY k; DWORD disp; DWORD merit=0x800002;
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,TEST_SCHEME,0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,L"Source Filter",0,REG_SZ,(BYTE*)CLSID_STR,(DWORD)((wcslen(CLSID_STR)+1)*2));
        RegSetValueExW(k,L"URL Protocol",0,REG_SZ,(BYTE*)L"",2); RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR,0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)L"Xune YouTube Stream Source",(DWORD)((wcslen(L"Xune YouTube Stream Source")+1)*2));
        RegSetValueExW(k,L"Merit",0,REG_DWORD,(BYTE*)&merit,4); RegCloseKey(k);
    }
    if(RegCreateKeyExW(HKEY_CLASSES_ROOT,L"CLSID\\" CLSID_STR L"\\InprocServer32",0,NULL,0,0xF003F,NULL,&k,&disp)==0){
        RegSetValueExW(k,NULL,0,REG_SZ,(BYTE*)DLL_PATH,(DWORD)((wcslen(DLL_PATH)+1)*2));
        RegSetValueExW(k,L"ThreadingModel",0,REG_SZ,(BYTE*)L"Both",10); RegCloseKey(k);
    }
}

extern "C" __declspec(dllexport) int RunDaemon(const void *arg, int arg_len, HANDLE stop_event){
    typedef int (*play_url_fn)(const wchar_t*, const wchar_t*);
    typedef int (*get_int_fn)(int*);
    char line[200]; HMODULE zdk; int hr, i;
    play_url_fn play; get_int_fn getpos, getcount, getidx;
    wchar_t url[80];
    (void)arg;(void)arg_len;
    L("=== ce_dshow_ytmstream: register ytm:// -> PlaySongFromURL(streaming) ===");
    init_vtbls();
    reg_setup();
    L("registry written");
    zdk=LoadLibraryW(L"zdksystem.dll");
    if(!zdk){ L("LoadLibrary(zdksystem) failed"); return -2; }
    play    =(play_url_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_PlaySongFromURL");
    getpos  =(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetPlayPosition");
    getcount=(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetSongCount");
    getidx  =(get_int_fn)GetProcAddress(zdk,L"ZDKMedia_Queue_GetActiveSongIndex");
    if(!play){ L("PlaySongFromURL unresolved"); return -3; }
    // arg (utf-8 videoId) overrides the default; else TEST_URL
    wcscpy(url,TEST_URL);
    if(arg && arg_len>0 && arg_len<60){ int j; const char* a=(const char*)arg; wchar_t* w=url; const wchar_t* pfx=L"ytm://"; int pi=0; while(pfx[pi]){*w++=pfx[pi++];} for(j=0;j<arg_len;j++) *w++=(wchar_t)(unsigned char)a[j]; *w=0; }
    { char b[100]; int j=0; while(url[j]&&j<80){b[j]=(char)url[j];j++;} b[j]=0; _snprintf(line,sizeof(line),"calling PlaySongFromURL(\"%s\") ...",b); L(line); }
    hr=play(L"YouTube Music", url);
    _snprintf(line,sizeof(line),"PlaySongFromURL hr=0x%08x (0=ok)",hr); L(line);
    for(i=0;i<120;i++){
        int pos=-1,cnt=-1,idx=-1;
        if(WaitForSingleObject(stop_event,2000)==WAIT_OBJECT_0){ L("stop signalled"); break; }
        if(getpos)getpos(&pos); if(getcount)getcount(&cnt); if(getidx)getidx(&idx);
        _snprintf(line,sizeof(line),"  t+%ds pos_ms=%d count=%d idx=%d",(i+1)*2,pos,cnt,idx); L(line);
    }
    L("--- exit (scheme left registered)");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h,DWORD r,LPVOID l){(void)h;(void)l; if(r==DLL_PROCESS_ATTACH) init_vtbls(); return TRUE;}
