/* ce_image.h: delegating image decoder for Zune HD.
 *
 * Wraps the CE 6 IImagingFactory COM API (imaging.dll, on-device since v4.5)
 * to provide JPEG/PNG/GIF/BMP/TIFF decode to a single RGBA8888 buffer.
 * Replaces libjpeg-turbo + libpng + libnsgif + libnsbmp in the NetSurf
 * content/handlers/ pipeline.
 */
#ifndef CE_IMAGE_H
#define CE_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CE_IMAGE_OK = 0,
    CE_IMAGE_ERR_INVALID = 1,
    CE_IMAGE_ERR_COM = 2,
    CE_IMAGE_ERR_FACTORY = 3,
    CE_IMAGE_ERR_CREATE = 4,
    CE_IMAGE_ERR_INFO = 5,
    CE_IMAGE_ERR_BITMAP = 6,
    CE_IMAGE_ERR_LOCK = 7,
    CE_IMAGE_ERR_ALLOC = 8,
    CE_IMAGE_ERR_ENCODER = 9,   /* no matching installed encoder */
    CE_IMAGE_ERR_ENCODE = 10    /* encode pipeline (create/sink/push) failed */
} ce_image_status;

/* Decode buf[0..size) into a freshly-allocated RGBA8888 byte array.
 * On success, *rgba_out is owned by the caller and must be released with
 * ce_image_free. rgba_out stride is width*4. */
ce_image_status ce_image_decode(const void *buf, unsigned int size,
                                unsigned char **rgba_out,
                                unsigned int *width_out,
                                unsigned int *height_out);

void ce_image_free(unsigned char *rgba);

/* Decode buf[0..size), downscale so the longest side is <= max_dim (aspect
 * preserved; never upscales), and re-encode as JPEG. On success *jpeg_out is a
 * freshly-allocated JPEG byte array of *jpeg_len_out bytes, owned by the caller
 * (release with ce_image_free). Uses the same on-device IImagingFactory
 * (imaging.dll) for decode, scale, and encode. */
ce_image_status ce_image_thumbnail_jpeg(const void *buf, unsigned int size,
                                        unsigned int max_dim,
                                        unsigned char **jpeg_out,
                                        unsigned int *jpeg_len_out);

/* Encode a raw 32bpp pixel buffer directly to JPEG, no intermediate decode.
 * Pixels are B,G,R,A byte order (PixelFormat32bppARGB, the Zune framebuffer
 * layout); stride is the row byte stride (>= width*4). quality is the JPEG
 * quality 0..100 (set via the imaging.dll EncoderQuality parameter). On success
 * *jpeg_out is a freshly-allocated JPEG byte array of *jpeg_len_out bytes, owned
 * by the caller (release with ce_image_free). */
ce_image_status ce_image_encode_jpeg(const void *pixels,
                                     unsigned int width, unsigned int height,
                                     unsigned int stride, unsigned int quality,
                                     unsigned char **jpeg_out,
                                     unsigned int *jpeg_len_out);

#ifdef __cplusplus
}
#endif

#endif /* CE_IMAGE_H */
