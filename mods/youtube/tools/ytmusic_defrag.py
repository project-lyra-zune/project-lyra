#!/usr/bin/env python3
# fMP4 (DASH audio) -> plain MP4 de-fragmenter.
#
# YouTube serves audio (itag 140 AAC-LC) as fragmented MP4 (moof/mdat), which the
# Zune HD's 2009 parser cannot read. This rewrites it losslessly into a plain
# MP4 (single moov+mdat) the Zune plays via ZDKMedia_Queue_PlaySongFromFile: it
# concatenates the moof/trun AAC samples, synthesizes a normal stbl (sizes from
# the truns, uniform durations), and copies the stsd (AAC esds) verbatim from the
# init moov. The AAC bitstream is untouched; no transcode.
#
# Reference for the on-device C port (zune-browser/src). Device-validated: output
# plays via PlaySongFromFile (count=1, position advances 1:1).
import struct, sys

def boxes(buf, start, end):
    o = start
    while o + 8 <= end:
        size = struct.unpack(">I", buf[o:o+4])[0]
        typ = buf[o+4:o+8]
        if size == 1:
            size = struct.unpack(">Q", buf[o+8:o+16])[0]
        if size < 8: break
        yield typ, o, o+size
        o += size

def children(buf, box_start, box_end):
    return list(boxes(buf, box_start+8, box_end))

def main(inp, outp):
    d = open(inp, "rb").read()
    top = list(boxes(d, 0, len(d)))
    ftyp = next((s,e) for t,s,e in top if t==b'ftyp')
    moov0 = next((s,e) for t,s,e in top if t==b'moov')

    # --- walk init moov -> trak -> mdia(mdhd timescale, hdlr) -> minf -> stbl(stsd) ---
    def child(s,e,typ):
        for t,bs,be in children(d,s,e):
            if t==typ: return (bs,be)
        return None
    trak = child(*moov0, b'trak')
    mdia = child(*trak, b'mdia')
    mdhd = child(*mdia, b'mdhd')
    # mdhd v0: ver(1)flags(3)ctime(4)mtime(4)timescale(4)duration(4)
    ver = d[mdhd[0]+8]
    if ver==0:
        timescale = struct.unpack(">I", d[mdhd[0]+8+12:mdhd[0]+8+16])[0]
    else:
        timescale = struct.unpack(">I", d[mdhd[0]+8+20:mdhd[0]+8+24])[0]
    minf = child(*mdia, b'minf')
    stbl = child(*minf, b'stbl')
    stsd = child(*stbl, b'stsd')
    stsd_bytes = d[stsd[0]:stsd[1]]   # copy verbatim (carries AAC esds)

    # --- walk fragments: collect sample sizes + durations + sample bytes ---
    sizes=[]; durs=[]; sample_data=bytearray()
    # default sample duration/size from tfhd (per traf); trun overrides per-sample
    for t,s,e in top:
        if t!=b'moof': continue
        traf = child(s,e,b'traf')
        tfhd = child(*traf,b'tfhd')
        tf_flags = struct.unpack(">I", d[tfhd[0]+8:tfhd[0]+12])[0] & 0xffffff
        p = tfhd[0]+12+4  # skip track_ID
        def_dur=def_size=None
        if tf_flags & 0x000001: p+=8           # base-data-offset
        if tf_flags & 0x000002: p+=4           # sample-description-index
        if tf_flags & 0x000008: def_dur=struct.unpack(">I",d[p:p+4])[0]; p+=4
        if tf_flags & 0x000010: def_size=struct.unpack(">I",d[p:p+4])[0]; p+=4
        # mdat for this fragment
        mdat = child(s,e,b'mdat')
        if mdat is None:
            # mdat is a sibling of moof, right after it
            mdat = next(((bs,be) for tt,bs,be in top if tt==b'mdat' and bs>e-1), None)
        # parse trun(s)
        for tt,bs,be in children(d,*traf):
            if tt!=b'trun': continue
            flags = struct.unpack(">I",d[bs+8:bs+12])[0] & 0xffffff
            cnt = struct.unpack(">I",d[bs+12:bs+16])[0]
            q = bs+16
            if flags & 0x000001: q+=4   # data-offset
            if flags & 0x000004: q+=4   # first-sample-flags
            for i in range(cnt):
                sd = def_dur; ss = def_size
                if flags & 0x000100: sd=struct.unpack(">I",d[q:q+4])[0]; q+=4
                if flags & 0x000200: ss=struct.unpack(">I",d[q:q+4])[0]; q+=4
                if flags & 0x000400: q+=4
                if flags & 0x000800: q+=4
                sizes.append(ss); durs.append(sd if sd is not None else 1024)
        # collect this fragment's mdat payload (its samples, contiguous)
        # find the mdat immediately following this moof in top order
    # second pass for mdat payloads (concat all mdat box payloads in order)
    for t,s,e in top:
        if t==b'mdat':
            sample_data += d[s+8:e]

    total_dur = sum(durs)
    n = len(sizes)

    # --- build boxes ---
    def box(typ, payload): return struct.pack(">I",8+len(payload))+typ+payload
    def fullbox(typ, ver, flags, payload): return box(typ, struct.pack(">I",(ver<<24)|flags)+payload)

    # stts: run-length encode durations
    runs=[]
    for dd in durs:
        if runs and runs[-1][1]==dd: runs[-1][0]+=1
        else: runs.append([1,dd])
    stts_payload = struct.pack(">I",len(runs)) + b''.join(struct.pack(">II",c,v) for c,v in runs)
    stts = fullbox(b'stts',0,0,stts_payload)
    # stsc: all samples in one chunk
    stsc = fullbox(b'stsc',0,0, struct.pack(">I",1)+struct.pack(">III",1,n,1))
    # stsz: per-sample sizes
    stsz = fullbox(b'stsz',0,0, struct.pack(">II",0,n)+b''.join(struct.pack(">I",s) for s in sizes))
    # stco: filled after we know moov size (chunk offset = ftyp + moov + mdat header)
    stco_placeholder = fullbox(b'stco',0,0, struct.pack(">I",1)+struct.pack(">I",0))
    # assemble stbl
    stbl_new = box(b'stbl', stsd_bytes + stts + stsc + stsz + stco_placeholder)
    # rebuild minf in canonical order: smhd, dinf, stbl
    smhd_c = child(*minf, b'smhd'); dinf_c = child(*minf, b'dinf')
    smhd_b = d[smhd_c[0]:smhd_c[1]] if smhd_c else box(b'smhd', struct.pack(">IHH",0,0,0))
    dinf_b = d[dinf_c[0]:dinf_c[1]] if dinf_c else b''
    minf_new = box(b'minf', smhd_b + dinf_b + stbl_new)
    mdia_children=b''
    for tt,bs,be in children(d,*mdia):
        if tt==b'minf': mdia_children+=minf_new
        elif tt==b'mdhd':
            # rewrite duration (keep ver0 layout)
            mb=bytearray(d[bs:be])
            if mb[8]==0: struct.pack_into(">I",mb,8+16,total_dur & 0xffffffff)
            mdia_children+=bytes(mb)
        else: mdia_children+=d[bs:be]
    mdia_new = box(b'mdia', mdia_children)
    # movie timescale (needed for tkhd duration + elst segment_duration)
    mvhd0 = child(*moov0,b'mvhd')
    movie_ts = struct.unpack(">I",d[mvhd0[0]+8+12:mvhd0[0]+8+16])[0] if d[mvhd0[0]+8]==0 else 1000
    seg_movie = int(total_dur*movie_ts/timescale) & 0xffffffff
    # canonical edit list (no-op: full segment, media_time 0); textbook MP4 shape
    elst = fullbox(b'elst',0,0, struct.pack(">I",1)+struct.pack(">IiI", seg_movie, 0, 0x10000))
    edts = box(b'edts', elst)
    # trak: tkhd + edts + mdia (canonical order; drop any source edts/extras)
    tkhd_c = child(*trak,b'tkhd'); tb=bytearray(d[tkhd_c[0]:tkhd_c[1]])
    if tb[8]==0: struct.pack_into(">I",tb,8+20, seg_movie)
    trak_new = box(b'trak', bytes(tb) + edts + mdia_new)
    # moov: mvhd (rewrite duration to movie timescale) + trak ; DROP mvex
    mvhd0 = child(*moov0,b'mvhd')
    mvb=bytearray(d[mvhd0[0]:mvhd0[1]])
    movie_ts = struct.unpack(">I",mvb[8+12:8+16])[0] if mvb[8]==0 else 1000
    if mvb[8]==0: struct.pack_into(">I",mvb,8+16, int(total_dur*movie_ts/timescale) & 0xffffffff)
    moov_new = box(b'moov', bytes(mvb)+trak_new)

    ftyp_bytes = d[ftyp[0]:ftyp[1]]
    # now compute stco offset = len(ftyp)+len(moov)+8(mdat header)
    chunk_off = len(ftyp_bytes)+len(moov_new)+8
    # patch the stco value inside moov_new (find 'stco' box, last 4 bytes = offset)
    idx = moov_new.find(b'stco')
    struct.pack_into(">I", moov_new:=bytearray(moov_new), idx+4+8, chunk_off)
    mdat_new = struct.pack(">I",8+len(sample_data))+b'mdat'+bytes(sample_data)
    out = ftyp_bytes + bytes(moov_new) + mdat_new
    open(outp,"wb").write(out)
    print(f"samples={n} total_dur={total_dur} ts={timescale} dur_s={total_dur/timescale:.1f} out={len(out)}B chunk_off={chunk_off}")

if __name__=="__main__":
    main(sys.argv[1], sys.argv[2])
