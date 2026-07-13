# youtube

A native YouTube / YouTube Music mod for the Zune HD. It adds a YouTube tile to the
Start menu with a marketplace-style search/browse UI, and streams YouTube Music
itag-140 AAC through the device's native queue: a DirectShow source registered for
the `ytm://` scheme is CoCreated inside zmedia, fetches over HTTPS (`ce_https`),
de-frags the fragmented MP4 on the fly (`ce_mp4_defrag`), and pushes AAC access units
into the NVIDIA AAC decoder and renderer.

This is a rich mod: a gemstone UI DLL plus a daemon plus a DirectShow plugin. Its
networking, TLS, and CA bundle come from the tree's shared `src/ce-common`; it vendors
no crypto of its own.

## Layout

```
manifest.json                     capabilities: the YouTube tile, scene entries,
                                  the ytm:// COM registration, the ytsearchd daemon
scenes/*.xui                      scene sources (compiled to .xur at package time)
src/
  youtube_gem.cpp                 gemstone UI scene classes            -> youtube.dll
  ytsearchd/                      InnerTube search daemon              -> ytsearchd.exe
  ce_dshow_ytmstream_plugin.cpp   live ytm:// DirectShow source        -> ce_ytmstream.dll
  ce_dshow_*_plugin.cpp           reference/diagnostic sources (not shipped)
  ce_innertube/                   YouTube InnerTube JSON client
  ce_mp4/                         incremental fMP4 de-fragmenter
build/                            per-target nmake makefiles + build bats
tools/ytmusic_defrag.py           host-side de-frag reference / analysis
```

## Building

The three shipped binaries build on the Windows-CE VM (Phase 1); the build scripts
link the shared `src/ce-common` (which builds wolfSSL from source, see the top-level
BUILDING.md). From `build/`:

```
build_youtube_gem.bat               -> youtube.dll
build_ytsearchd.bat                 -> ytsearchd.exe
build_ce_dshow_ytmstream_plugin.bat -> ce_ytmstream.dll
```

Copy each output to the mod root, then publish through the repo
(`tools/publish-repo.sh`), which compiles the scenes to `.xur` and packages the
`.zmod`.
