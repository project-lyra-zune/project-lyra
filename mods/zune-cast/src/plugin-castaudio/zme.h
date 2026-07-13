#ifndef ZME_H
#define ZME_H

// Direct ZME0: broker reads (DeviceIoControl 0x201), bypassing ZDKMedia's
// stale GetPlayState cache. Same transport the device uses for live position
// (0x2b) / duration (0x2f); 0x29 is the play-state read (sibling of the 0x28
// pause write).

// Reads one broker method (kind=music). Returns 1 on a successful IOCTL with
// the HRESULT in *out_hr and the value in *out_val; 0 if the IOCTL failed.
int  zme_read(unsigned short method, unsigned int* out_hr, unsigned int* out_val);

// Same, but with an explicit kind at buf+0xc (volume read needs kind 6, not the
// music-session kind 1; sub_4197a6fc selects 6 for audio media types).
int  zme_read_kind(unsigned short method, unsigned int kind,
                   unsigned int* out_hr, unsigned int* out_val);

// ZHD0:/0x208 scalar getter (volume/lock/battery HUD service). Each method gates
// on an exact out-buffer length; the value is returned at buf+4.
int  zhd_read(unsigned short method, unsigned int buflen, unsigned int* out_val);

// ZAM0:/0x204 method 0x12: the live on-screen volume the hardware buttons drive.
// Fills current volume + max (typically 0..30). 1 on success. Read-only.
int  zam_volume(unsigned int* out_vol, unsigned int* out_max);
// Set the on-screen volume (ZAM0:/0x204 method 0x11, absolute). vol is 0..max.
int  zam_set_volume(unsigned int vol, unsigned int max);

// Live binary play-state via broker method 0x29 (device-validated: raw 3=play,
// 4=pause). The export ZDKMedia_Queue_GetPlayState reads a stale cache and must
// not be used. Returns: 1=playing, 2=paused, 0=stopped/unknown.
#define ZME_PS_UNKNOWN 0
#define ZME_PS_PLAYING 1
#define ZME_PS_PAUSED  2
int  zme_play_state(void);

#endif // ZME_H
