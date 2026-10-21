# Cvičení

Cvičení jsou seřazená od nejjednodušších. U každého je uvedeno, kde začít a jak ověřit, že funguje. Po každé změně spusť `cmake --build build && ctest --test-dir build`.

## Rozcvička

### 1. Statistika interpretu ⭐
Přidej do `VM` počítadlo vykonaných instrukcí a po skončení programu ho vypiš (např. volbou `--stats`).
- **Kde:** `src/vm/interp.cpp` (smyčka `for (;;)`), `src/main.cpp`.
- **Ověření:** `fib(20)` v `VmTest` by měl vykonat zhruba stovky tisíc instrukcí.
- **Bonus:** histogram podle opcode. Které instrukce jsou nejčastější?

### 2. Chybějící metody String ⭐
Doplň `String.repeat(I)`, `String.join(CharSequence, CharSequence[])` a `String.chars()`… (to poslední je těžší, ukáže ti, proč je Java knihovna tak velká).
- **Kde:** `src/framework/java_lang.cpp`, sekce `String`.
- **Ověření:** přidej volání do `tests/java/VmTest.java` a přegeneruj data (`tests/regenerate-data.sh`). Test porovnává výstup s JVM.

### 3. Nové barvy a tvary ⭐
Implementuj `Canvas.drawArc(RectF, float startAngle, float sweepAngle, boolean useCenter, Paint)`.
- **Kde:** `src/gfx/raster.cpp` (nová funkce: SDF výseče = SDF kruhu ∩ úhel), `src/framework/android.cpp` (registrace v `Canvas`), `sdk-stubs/android/graphics/Canvas.java`.
- **Ověření:** nakresli v `BallView` „pacmana“ a udělej screenshot (`--headless --screenshot`).

## Framework

### 4. CheckBox ⭐⭐
Přidej `android.widget.CheckBox` (dědí `Button`): `setChecked`, `isChecked`, `setOnCheckedChangeListener`. Kliknutí přepíná stav a vlevo se kreslí čtvereček s fajfkou.
- **Kde:** `android.cpp`: nový `Widget::CheckBox`, `onTouchEvent`, `drawTextView`.
- **Nápověda:** listener je rozhraní `CompoundButton$OnCheckedChangeListener` s metodou `onCheckedChanged(Landroid/widget/CompoundButton;Z)V`.

### 5. Gesto „dlouhé podržení“ ⭐⭐
`View.setOnLongClickListener`: když se prst nepohne 500 ms, zavolej `onLongClick(View)`.
- **Nápověda:** při `ACTION_DOWN` naplánuj přes `AndroidRuntime::post` interní callback. Při `MOVE` mimo View nebo při `UP` ho zruš.

### 6. Obrázky: Bitmap a ImageView ⭐⭐⭐
Načti PNG z APK (`res/drawable/...`) a vykresli ho.
- **Kroky:**
  1. Dekodér PNG: chunky `IHDR`, `IDAT` (zlib už máme), filtry řádků (None, Sub, Up, Average, Paeth). To je skvělé samostatné cvičení na formát souboru.
  2. `Raster::drawImage(...)` s bilineárním škálováním.
  3. `BitmapFactory.decodeStream`, `Canvas.drawBitmap` a `ImageView.setImageBitmap`.

### 7. XML layouty ⭐⭐⭐⭐
Nejdůležitější krok ke „skutečným“ aplikacím: `setContentView(R.layout.activity_main)`.
- **Kroky:**
  1. Parser `resources.arsc` (tabulka zdrojů: id `0x7f0b001c` → soubor `res/layout/activity_main.xml` nebo řetězec).
  2. Binární XML layoutu už umíme přečíst (`parseBinaryXml`). Napiš „inflater“, který pro každý element vytvoří View (`LinearLayout`, `TextView`…), nastaví atributy (`android:text`, `android:layout_width`, `android:orientation`, `android:id`…) a poskládá strom.
  3. `findViewById(R.id.foo)` už funguje přes `setId`.
- **Testovací APK** sestav ze skutečného Android projektu, nebo rozšiř `tools/mkmanifest.py`, aby uměl zapsat i layout.

## Virtuální stroj

### 8. Rychlejší volání: vtable ⭐⭐
`Class::dispatch` dnes hledá v hashovací tabulce. Skutečné VM mají **vtable**: každá virtuální metoda dostane při linkování index a volání je jen `klass->vtable[index]`.
- **Ověření:** změř `fib(30)` před změnou a po ní (`time`).

### 9. Lambdy (invokedynamic) ⭐⭐⭐
`dx` lambdy neumí, ale `d8` z Android SDK je přeloží („desugaring“) na obyčejné třídy. Zkus sestavit APK pomocí d8 (`--min-api 21`) a zjisti, co ještě chybí, např. `invoke-custom` při vyšším `--min-api`.

### 10. Vlákna ⭐⭐⭐⭐
`new Thread(runnable).start()` je dnes stub. Varianty:
- *Kooperativní:* vlákno je jen další interpret, který se střídá s hlavním (každých N instrukcí se přepne kontext). Bez skutečného paralelismu, takže GC zůstává jednoduchý.
- *Skutečná vlákna:* `std::thread` na každé Java vlákno. Pak potřebuješ zámky (`monitor-enter`/`exit`), bezpečné body pro GC (stop-the-world) a atomické operace. Tohle je náročné, ale přesně tak to dělá ART.

### 11. JIT ⭐⭐⭐⭐⭐
Pro odvážné: místo interpretace přelož často volanou metodu do strojového kódu. Nejjednodušší cesta je „template JIT“: každá instrukce = předem připravený kousek x86-64 kódu (`mmap` s `PROT_EXEC`). Na FreeBSD pozor na W^X: stránku nejdřív zapiš a pak ji přepni na spustitelnou.

## Projekty na víc večerů

- **Zvuk:** `android.media.MediaPlayer` / `SoundPool` přes SDL audio (WAV, pak OGG).
- **Ukládání dat:** `SharedPreferences` do souboru v `~/.droidvm/<package>/`.
- **Síť:** `java.net.URL` / `HttpURLConnection` nad sockety.
- **Výkon kreslení:** překreslovat jen „špinavé“ oblasti (dirty rectangles) místo celé obrazovky.
- **Porovnání s bsdroid:** spusť stejnou aplikaci v droidvm a v plném Androidu v bhyve a porovnej screenshoty. Které rozdíly v layoutu najdeš?
