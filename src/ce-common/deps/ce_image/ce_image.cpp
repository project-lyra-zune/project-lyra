/* ce_image.cpp: IImagingFactory adapter (CE 6).
 * Compiled as C++ so we can use imaging.h's COM interface declarations
 * directly; exports a C ABI for NetSurf content handlers to link against.
 */
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <imgguids.h>
#include <imaging.h>
#include <stdlib.h>
#include "ce_image.h"

extern "C" ce_image_status ce_image_decode(const void *buf, unsigned int size,
                                           unsigned char **rgba_out,
                                           unsigned int *width_out,
                                           unsigned int *height_out)
{
    if (!buf || !size || !rgba_out || !width_out || !height_out)
        return CE_IMAGE_ERR_INVALID;

    *rgba_out = NULL;
    *width_out = 0;
    *height_out = 0;

    /* CE 6 CoInitializeEx is a near-stub; S_FALSE means the thread was
     * already inited (still our responsibility to balance with Uninit).
     * RPC_E_CHANGED_MODE leaves the existing apartment alone; skip Uninit. */
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL needsUninit = SUCCEEDED(hrInit);

    ce_image_status status = CE_IMAGE_OK;
    HRESULT hr = S_OK;
    IImagingFactory *factory = NULL;
    IImage *image = NULL;
    IBitmapImage *bitmap = NULL;
    ImageInfo info;
    BitmapData data;
    RECT rect;
    BOOL locked = FALSE;
    unsigned char *out = NULL;
    const unsigned char *src = NULL;
    INT stride = 0;
    UINT y, x;

    memset(&info, 0, sizeof(info));
    memset(&data, 0, sizeof(data));
    memset(&rect, 0, sizeof(rect));

    hr = CoCreateInstance(CLSID_ImagingFactory, NULL,
                          CLSCTX_INPROC_SERVER,
                          IID_IImagingFactory,
                          (void **)&factory);
    if (FAILED(hr) || !factory) {
        status = CE_IMAGE_ERR_FACTORY;
        goto cleanup;
    }

    hr = factory->CreateImageFromBuffer(buf, size,
                                        BufferDisposalFlagNone, &image);
    if (FAILED(hr) || !image) {
        status = CE_IMAGE_ERR_CREATE;
        goto cleanup;
    }

    hr = image->GetImageInfo(&info);
    if (FAILED(hr) || info.Width == 0 || info.Height == 0) {
        status = CE_IMAGE_ERR_INFO;
        goto cleanup;
    }

    /* Force 32-bit ARGB regardless of source format. Opaque sources
     * (JPEG, alpha-less PNG) come back with A=0xFF for every pixel. */
    hr = factory->CreateBitmapFromImage(image, info.Width, info.Height,
                                        PixelFormat32bppARGB,
                                        InterpolationHintDefault,
                                        &bitmap);
    if (FAILED(hr) || !bitmap) {
        status = CE_IMAGE_ERR_BITMAP;
        goto cleanup;
    }

    rect.right = (LONG)info.Width;
    rect.bottom = (LONG)info.Height;
    hr = bitmap->LockBits(&rect, ImageLockModeRead,
                          PixelFormat32bppARGB, &data);
    if (FAILED(hr)) {
        status = CE_IMAGE_ERR_LOCK;
        goto cleanup;
    }
    locked = TRUE;

    out = (unsigned char *)malloc((size_t)info.Width * info.Height * 4u);
    if (!out) {
        status = CE_IMAGE_ERR_ALLOC;
        goto cleanup;
    }

    /* PixelFormat32bppARGB lays bytes out as B,G,R,A in memory (GDI+
     * convention); NetSurf wants R,G,B,A. Swizzle row-by-row to honor
     * Scan0 stride, which may exceed Width*4 for alignment. */
    src = (const unsigned char *)data.Scan0;
    stride = data.Stride;
    for (y = 0; y < info.Height; ++y) {
        const unsigned char *sp = src + (INT)y * stride;
        unsigned char *dp = out + (size_t)y * info.Width * 4u;
        for (x = 0; x < info.Width; ++x) {
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
            sp += 4;
            dp += 4;
        }
    }

    *rgba_out = out;
    *width_out = info.Width;
    *height_out = info.Height;
    out = NULL;

cleanup:
    if (locked)   bitmap->UnlockBits(&data);
    if (bitmap)   bitmap->Release();
    if (image)    image->Release();
    if (factory)  factory->Release();
    if (out)      free(out);
    if (needsUninit) CoUninitialize();
    return status;
}

extern "C" ce_image_status ce_image_thumbnail_jpeg(const void *buf, unsigned int size,
                                                   unsigned int max_dim,
                                                   unsigned char **jpeg_out,
                                                   unsigned int *jpeg_len_out)
{
    if (!buf || !size || !max_dim || !jpeg_out || !jpeg_len_out)
        return CE_IMAGE_ERR_INVALID;

    *jpeg_out = NULL;
    *jpeg_len_out = 0;

    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL needsUninit = SUCCEEDED(hrInit);

    ce_image_status status = CE_IMAGE_OK;
    HRESULT hr = S_OK;
    IImagingFactory *factory = NULL;
    IImage *image = NULL;
    IBitmapImage *bitmap = NULL;
    IStream *stream = NULL;
    IImageEncoder *encoder = NULL;
    IImageSink *sink = NULL;
    ImageInfo info;
    ImageInfo sinkInfo;
    BitmapData data;
    RECT rect;
    BOOL locked = FALSE;
    HGLOBAL hg = NULL;
    void *hgptr = NULL;
    unsigned char *out = NULL;
    UINT tw, th;
    STATSTG stat;
    CLSID encClsid;
    BOOL haveEnc = FALSE;
    UINT encCount = 0;
    ImageCodecInfo *codecs = NULL;

    memset(&encClsid, 0, sizeof(encClsid));

    memset(&info, 0, sizeof(info));
    memset(&data, 0, sizeof(data));
    memset(&rect, 0, sizeof(rect));
    memset(&stat, 0, sizeof(stat));

    hr = CoCreateInstance(CLSID_ImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_IImagingFactory, (void **)&factory);
    if (FAILED(hr) || !factory) { status = CE_IMAGE_ERR_FACTORY; goto cleanup; }

    hr = factory->CreateImageFromBuffer(buf, size, BufferDisposalFlagNone, &image);
    if (FAILED(hr) || !image) { status = CE_IMAGE_ERR_CREATE; goto cleanup; }

    hr = image->GetImageInfo(&info);
    if (FAILED(hr) || info.Width == 0 || info.Height == 0) { status = CE_IMAGE_ERR_INFO; goto cleanup; }

    /* Downscale so the longest side is max_dim, preserving aspect (never up). */
    tw = info.Width;
    th = info.Height;
    {
        UINT longest = (info.Width > info.Height) ? info.Width : info.Height;
        if (longest > max_dim) {
            tw = (UINT)((double)info.Width  * max_dim / longest + 0.5);
            th = (UINT)((double)info.Height * max_dim / longest + 0.5);
            if (tw == 0) tw = 1;
            if (th == 0) th = 1;
        }
    }

    hr = factory->CreateBitmapFromImage(image, tw, th, PixelFormat32bppARGB,
                                        InterpolationHintDefault, &bitmap);
    if (FAILED(hr) || !bitmap) { status = CE_IMAGE_ERR_BITMAP; goto cleanup; }

    rect.right  = (LONG)tw;
    rect.bottom = (LONG)th;
    hr = bitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data);
    if (FAILED(hr)) { status = CE_IMAGE_ERR_LOCK; goto cleanup; }
    locked = TRUE;

    /* CreateImageEncoderToStream needs the ENCODER's CLSID, which differs from
     * the format GUID; enumerate installed encoders and match by FormatID. */
    hr = factory->GetInstalledEncoders(&encCount, &codecs);
    if (FAILED(hr) || !codecs) { status = CE_IMAGE_ERR_ENCODER; goto cleanup; }
    for (UINT k = 0; k < encCount; ++k) {
        if (IsEqualGUID(codecs[k].FormatID, ImageFormatJPEG)) {
            encClsid = codecs[k].Clsid; haveEnc = TRUE; break;
        }
    }
    if (!haveEnc) { status = CE_IMAGE_ERR_ENCODER; goto cleanup; }

    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr) || !stream) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hr = factory->CreateImageEncoderToStream(&encClsid, stream, &encoder);
    if (FAILED(hr) || !encoder) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }

    hr = encoder->GetEncodeSink(&sink);
    if (FAILED(hr) || !sink) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }

    /* Drive the encoder sink with the downscaled pixels (IImage::PushIntoSink
     * is not available on IBitmapImage, so push raw pixel data directly). */
    sinkInfo = info;
    sinkInfo.Width = tw;
    sinkInfo.Height = th;
    sinkInfo.PixelFormat = PixelFormat32bppARGB;
    hr = sink->BeginSink(&sinkInfo, NULL);
    if (FAILED(hr)) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }
    hr = sink->PushPixelData(&rect, &data, TRUE);
    if (FAILED(hr)) { sink->EndSink(hr); status = CE_IMAGE_ERR_ENCODE; goto cleanup; }
    sink->EndSink(S_OK);
    encoder->TerminateEncoder();

    /* cbSize is bytes actually written (the HGLOBAL is over-allocated). */
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.LowPart == 0) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hr = GetHGlobalFromStream(stream, &hg);
    if (FAILED(hr) || !hg) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hgptr = GlobalLock(hg);
    if (!hgptr) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    out = (unsigned char *)malloc(stat.cbSize.LowPart);
    if (!out) { GlobalUnlock(hg); status = CE_IMAGE_ERR_ALLOC; goto cleanup; }
    memcpy(out, hgptr, stat.cbSize.LowPart);
    GlobalUnlock(hg);
    *jpeg_out = out;
    *jpeg_len_out = stat.cbSize.LowPart;
    out = NULL;

cleanup:
    if (codecs)   CoTaskMemFree(codecs);
    if (locked)   bitmap->UnlockBits(&data);
    if (sink)     sink->Release();
    if (encoder)  encoder->Release();
    if (stream)   stream->Release();
    if (bitmap)   bitmap->Release();
    if (image)    image->Release();
    if (factory)  factory->Release();
    if (out)      free(out);
    if (needsUninit) CoUninitialize();
    return status;
}

extern "C" ce_image_status ce_image_encode_jpeg(const void *pixels,
                                                unsigned int width, unsigned int height,
                                                unsigned int stride, unsigned int quality,
                                                unsigned char **jpeg_out,
                                                unsigned int *jpeg_len_out)
{
    if (!pixels || !width || !height || stride < width * 4u || !jpeg_out || !jpeg_len_out)
        return CE_IMAGE_ERR_INVALID;

    *jpeg_out = NULL;
    *jpeg_len_out = 0;

    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL needsUninit = SUCCEEDED(hrInit);

    ce_image_status status = CE_IMAGE_OK;
    HRESULT hr = S_OK;
    IImagingFactory *factory = NULL;
    IStream *stream = NULL;
    IImageEncoder *encoder = NULL;
    IImageSink *sink = NULL;
    ImageInfo sinkInfo;
    BitmapData data;
    RECT rect;
    HGLOBAL hg = NULL;
    void *hgptr = NULL;
    unsigned char *out = NULL;
    STATSTG stat;
    CLSID encClsid;
    BOOL haveEnc = FALSE;
    UINT encCount = 0;
    ImageCodecInfo *codecs = NULL;
    EncoderParameters encParams;
    ULONG qval;

    memset(&encClsid, 0, sizeof(encClsid));
    memset(&sinkInfo, 0, sizeof(sinkInfo));
    memset(&data, 0, sizeof(data));
    memset(&rect, 0, sizeof(rect));
    memset(&stat, 0, sizeof(stat));

    hr = CoCreateInstance(CLSID_ImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_IImagingFactory, (void **)&factory);
    if (FAILED(hr) || !factory) { status = CE_IMAGE_ERR_FACTORY; goto cleanup; }

    hr = factory->GetInstalledEncoders(&encCount, &codecs);
    if (FAILED(hr) || !codecs) { status = CE_IMAGE_ERR_ENCODER; goto cleanup; }
    for (UINT k = 0; k < encCount; ++k) {
        if (IsEqualGUID(codecs[k].FormatID, ImageFormatJPEG)) {
            encClsid = codecs[k].Clsid; haveEnc = TRUE; break;
        }
    }
    if (!haveEnc) { status = CE_IMAGE_ERR_ENCODER; goto cleanup; }

    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr) || !stream) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hr = factory->CreateImageEncoderToStream(&encClsid, stream, &encoder);
    if (FAILED(hr) || !encoder) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }

    /* JPEG quality via the EncoderQuality parameter (ULONG 0..100). Best-effort:
     * a codec that ignores it just encodes at its default. Must be set before
     * GetEncodeSink/BeginSink. */
    if (quality > 100) quality = 100;
    qval = quality;
    encParams.Count = 1;
    encParams.Parameter[0].Guid = EncoderQuality;
    encParams.Parameter[0].NumberOfValues = 1;
    encParams.Parameter[0].Type = 4;   /* EncoderParameterValueTypeLong (gdiplusenums.h) */
    encParams.Parameter[0].Value = &qval;
    encoder->SetEncoderParameters(&encParams);

    hr = encoder->GetEncodeSink(&sink);
    if (FAILED(hr) || !sink) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }

    /* Describe the raw frame. SinkFlagsTopDown: Scan0 is the top row (the Zune
     * framebuffer's row 0), so the encoder must not assume bottom-up BMP order. */
    sinkInfo.RawDataFormat = ImageFormatMemoryBMP;
    sinkInfo.PixelFormat = PixelFormat32bppARGB;
    sinkInfo.Width = width;
    sinkInfo.Height = height;
    sinkInfo.Flags = SinkFlagsTopDown;

    rect.right = (LONG)width;
    rect.bottom = (LONG)height;

    data.Width = width;
    data.Height = height;
    data.Stride = (INT)stride;
    data.PixelFormat = PixelFormat32bppARGB;
    data.Scan0 = (VOID *)pixels;

    hr = sink->BeginSink(&sinkInfo, NULL);
    if (FAILED(hr)) { status = CE_IMAGE_ERR_ENCODE; goto cleanup; }
    hr = sink->PushPixelData(&rect, &data, TRUE);
    if (FAILED(hr)) { sink->EndSink(hr); status = CE_IMAGE_ERR_ENCODE; goto cleanup; }
    sink->EndSink(S_OK);
    encoder->TerminateEncoder();

    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.LowPart == 0) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hr = GetHGlobalFromStream(stream, &hg);
    if (FAILED(hr) || !hg) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    hgptr = GlobalLock(hg);
    if (!hgptr) { status = CE_IMAGE_ERR_ALLOC; goto cleanup; }

    out = (unsigned char *)malloc(stat.cbSize.LowPart);
    if (!out) { GlobalUnlock(hg); status = CE_IMAGE_ERR_ALLOC; goto cleanup; }
    memcpy(out, hgptr, stat.cbSize.LowPart);
    GlobalUnlock(hg);
    *jpeg_out = out;
    *jpeg_len_out = stat.cbSize.LowPart;
    out = NULL;

cleanup:
    if (codecs)   CoTaskMemFree(codecs);
    if (sink)     sink->Release();
    if (encoder)  encoder->Release();
    if (stream)   stream->Release();
    if (factory)  factory->Release();
    if (out)      free(out);
    if (needsUninit) CoUninitialize();
    return status;
}

extern "C" void ce_image_free(unsigned char *rgba)
{
    free(rgba);
}
