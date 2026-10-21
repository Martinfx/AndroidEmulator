# bsdroid – plný Android s Google Play na FreeBSD

`bsdroid` je jeden shellový skript, který na FreeBSD spustí kompletní Android (BlissOS nebo Android-x86 s Google aplikacemi) jako virtuální stroj v **bhyve**. V tomto Androidu se můžeš přihlásit Google účtem a instalovat aplikace z Google Play.

> **Stav:** skript je napsaný podle dokumentace bhyve a BlissOS. Na skutečném FreeBSD zatím **nebyl otestovaný**.

## Požadavky

- FreeBSD 13.2+ nebo 14.x (amd64), CPU s VT-x/AMD-V (v BIOSu zapnuté), aspoň 8 GB RAM
- ISO **BlissOS s GApps** (https://blissos.org, varianta „GApps“, x86_64). Funguje i Android-x86 9.0 s OpenGApps.

## Instalace

```sh
doas install -m 755 bsdroid /usr/local/bin/bsdroid
doas bsdroid setup          # moduly jádra, balíčky, síťový bridge, dnsmasq
```

`setup` nainstaluje `bhyve-firmware`, `dnsmasq`, `tigervnc-viewer` a `android-tools`, vytvoří bridge `bridge77` (10.77.0.1/24) a vypíše pravidlo NAT pro `pf`, které musíš přidat do `/etc/pf.conf`. Bez NATu nebude Android mít internet, a tedy ani Google Play.

Volitelně vytvoř `/usr/local/etc/bsdroid.conf`:

```sh
BSDROID_ZFS=zroot/bsdroid    # zapne snapshoty přes ZFS
BSDROID_DIR=/usr/local/bsdroid
```

## Použití

```sh
doas bsdroid create play -m 6G -c 4 -d 32G -r 1280x720
doas bsdroid install play ~/Downloads/Bliss-v16-x86_64-OFFICIAL-gapps.iso
#   -> otevře se VNC okno s instalátorem, nainstaluj na disk (GPT, ext4, EFI GRUB)
doas bsdroid start play
bsdroid vnc play                         # obrazovka
bsdroid apk play ~/Downloads/app.apk     # instalace APK přes adb
bsdroid adb play shell                   # shell v Androidu
doas bsdroid stop play
doas bsdroid snapshot play cisty         # jen se ZFS
doas bsdroid rollback play cisty
bsdroid list
```

## Přihlášení do Google Play

Google blokuje přihlášení na necertifikovaných zařízeních, což je i každý virtuální stroj. Postup:

1. Nastartuj VM, otevři Play Store (přihlášení zatím může selhat).
2. `bsdroid gsf-id play` vypíše GSF Android ID.
3. Zaregistruj ho na https://www.google.com/android/uncertified (pod svým Google účtem).
4. Po pár minutách restartuj VM (`stop` + `start`) a přihlas se.

## Známá omezení

- **Grafika je softwarová.** bhyve nemá virtuální GPU s 3D akcelerací, takže Android vykresluje přes SwiftShader nebo llvmpipe. Běžné aplikace jsou použitelné, 3D hry budou pomalé.
- Pokud po startu zůstane černá obrazovka, v GRUBu stiskni `e` a na konec řádku s `linux` (kernel) přidej `nomodeset HWACCEL=0`.
- Aplikace jen pro ARM poběží jen tehdy, pokud obraz obsahuje překladač ARM→x86 (libhoudini nebo libndk). BlissOS ho obvykle obsahuje.
- Rozlišení je omezené na 1920×1200 (limit framebufferu v bhyve).
- Některé aplikace (bankovnictví, Netflix) odmítnou běžet kvůli Play Integrity.
