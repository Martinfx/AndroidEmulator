# Android na FreeBSD

Dva projekty, dvě cesty ke spouštění Android aplikací na FreeBSD:

| | [bsdroid](bsdroid/) | [droidvm](droidvm/) |
|---|---|---|
| Co to je | spouštěč plného Androidu (BlissOS) v hypervisoru bhyve | vlastní emulátor napsaný od nuly v C++ |
| Google Play, přihlášení | ano | ne |
| Běžné aplikace z obchodu | ano (grafika je softwarová) | ne, jen jednoduché aplikace s UI v kódu |
| K čemu | používat aplikace | naučit se, jak Android funguje uvnitř |

- `bsdroid/bsdroid` – shellový skript: `setup`, `create`, `install`, `start`, `vnc`, `apk`, `gsf-id`, snapshoty ZFS. Návod v [bsdroid/README.md](bsdroid/README.md).
- `droidvm/` – APK/DEX parser, interpret Dalvik bajtkódu, garbage collector, Android framework (Activity, View, Canvas…), softwarový rasterizér a SDL2 okno. Průvodce kódem: [droidvm/docs/navod.md](droidvm/docs/navod.md), cvičení: [droidvm/docs/cviceni.md](droidvm/docs/cviceni.md).
