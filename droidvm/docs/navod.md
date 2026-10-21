# Průvodce: jak funguje Android emulátor od nuly

Tento průvodce tě provede celým `droidvm` od souboru APK až po pixel na obrazovce. Ke každé vrstvě uvádí, ve kterém souboru ji najdeš, co si přečíst a jaký pokus si můžeš udělat. Nejlíp se čte s otevřeným kódem vedle.

```
 app.apk ──► ZIP ──► AndroidManifest.xml (binární XML) ──► která aktivita se spustí
              │
              └────► classes.dex ──► třídy, metody, bajtkód
                                          │
                                          ▼
                                interpret Dalvik bajtkódu ◄──► nativní java.lang / java.util
                                          │
                                          ▼
                        Android framework (Activity, View, Canvas…)
                                          │
                                          ▼
                     softwarový rasterizér ──► framebuffer ──► SDL2 okno
```

| Vrstva | Soubor | Řádků |
|---|---|---|
| ZIP / APK | `src/apk/zip.cpp` | ~90 |
| Binární XML | `src/apk/axml.cpp` | ~150 |
| Formát DEX | `src/apk/dex.cpp` | ~300 |
| Třídy, objekty, GC | `src/vm/vm.cpp`, `vm.h` | ~900 |
| Interpret | `src/vm/interp.cpp` | ~500 |
| java.lang, java.util | `src/framework/java_lang.cpp` | ~1050 |
| Android framework | `src/framework/android.cpp` | ~1600 |
| Rasterizér | `src/gfx/raster.cpp` | ~250 |
| Hlavní smyčka, SDL | `src/main.cpp` | ~280 |

---

## 1. APK je obyčejný ZIP

```sh
unzip -l tests/data/ahoj.apk
```

Uvidíš `classes.dex` (kód) a `AndroidManifest.xml` (popis aplikace). Skutečné APK obsahují navíc `resources.arsc`, `res/` (obrázky, layouty), `lib/` (nativní `.so` knihovny) a `META-INF/` (podpisy).

**Jak se ZIP čte** (`zip.cpp`): ZIP se čte *odzadu*. Na konci souboru leží záznam *End of Central Directory* se signaturou `0x06054b50`, který říká, kde začíná *centrální adresář*, tedy seznam všech souborů s jejich velikostí, kompresí a offsetem. U každého souboru je pak ještě *lokální hlavička* a za ní data, buď nekomprimovaná (metoda 0), nebo komprimovaná algoritmem deflate (metoda 8, rozbalí ho zlib).

> **Pokus:** `hexdump -C tests/data/ahoj.apk | tail -3` a najdi bajty `50 4b 05 06` (signatura, little-endian).

## 2. Binární XML

Android neukládá XML jako text, `aapt` ho při sestavení „zkompiluje“ do binární podoby (`axml.cpp`). Soubor je řada **chunků**. Každý začíná hlavičkou `typ (2 B), velikost hlavičky (2 B), velikost chunku (4 B)`:

- `0x0001` **string pool**: všechny řetězce dokumentu na jednom místě, ostatní na ně odkazují indexem,
- `0x0102` začátek elementu (jméno + atributy jako indexy do string poolu),
- `0x0103` konec elementu.

Z manifestu nás zajímá jediná věc: která `<activity>` má `intent-filter` s akcí `android.intent.action.MAIN` a kategorií `LAUNCHER`. Tu spouští launcher. `tools/mkmanifest.py` dělá opak: takový soubor vyrobí.

## 3. Formát DEX

`classes.dex` obsahuje všechny třídy aplikace (`dex.cpp`). Hlavička na začátku (`dex\n035\0`…) obsahuje offsety a velikosti tabulek:

| Tabulka | Obsah |
|---|---|
| `string_ids` | všechny řetězce (jména, popisy typů, literály) |
| `type_ids` | typy jako indexy do řetězců, např. `Ljava/lang/String;` |
| `proto_ids` | prototypy metod: návratový typ + parametry |
| `field_ids` | (třída, typ, jméno) každého pole |
| `method_ids` | (třída, prototyp, jméno) každé metody |
| `class_defs` | definice tříd: rodič, rozhraní, pole, metody a jejich kód |

Instrukce odkazují na tyto tabulky čísly, takže `invoke-static {v0}, method@12` znamená zavolat metodu č. 12.

Popisy typů (**descriptory**) jsou stejné jako v JVM: `I` = int, `J` = long, `Z` = boolean, `F` = float, `D` = double, `V` = void, `[I` = int[], `Lcz/priklad/Foo;` = třída. Signatura `(ILjava/lang/String;)V` je metoda `void f(int, String)`.

Dvě technické zvláštnosti:

- **ULEB128**: čísla proměnné délky. Každý bajt nese 7 bitů a nejvyšší bit říká „pokračuje se“. Viz `Reader::uleb()`.
- **MUTF-8**: řetězce jsou v „modifikovaném UTF-8“ (znaky mimo BMP jako dvojice surrogate, `\0` dvěma bajty). Uvnitř VM držíme řetězce v UTF-16 jako Java (`std::u16string`).

## 4. Dalvik je registrový stroj

JVM je *zásobníkový* stroj (`iload_0; iconst_2; if_icmpge`…). Dalvik je *registrový*: každá metoda si řekne, kolik registrů potřebuje, a instrukce s nimi pracují přímo. Argumenty dostane v **posledních** registrech (v disassembleru se jmenují `p0`, `p1`…).

Takhle vypadá `fib()` z `tests/java/VmTest.java`:

```java
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
```

přeložená nástrojem `dx` (výpis z `baksmali`):

```
.method static fib(I)I
    .registers 3            ; v0, v1, v2 (= p0 = n)
    const/4 v0, 0x2         ; 2012        v0 = 2
    if-ge p0, v0, :cond_4   ; 0235 0003   if (n >= 2) skoč o 3 jednotky dál (na adresu 4)
    :goto_3
    return p0               ; 020f
    :cond_4
    add-int/lit8 v0, p0, -0x1        ; 00d8 ff02
    invoke-static {v0}, LVmTest;->fib(I)I   ; 1071 xxxx 0000
    move-result v0                   ; 000a
    add-int/lit8 v1, p0, -0x2        ; 01d8 fe02
    invoke-static {v1}, LVmTest;->fib(I)I
    move-result v1
    add-int p0, v0, v1               ; 0290 0100
    goto :goto_3
.end method
```

Instrukce se skládá z 16bitových **kódových jednotek**. Spodní bajt první jednotky je **opcode** a zbytek kóduje registry a konstanty v jednom z ~30 **formátů**. Jméno formátu `22b` znamená 2 jednotky, 2 registry a bajtovou konstantu. Například `add-int/lit8 v0, p0, -1`:

```
00d8  →  AA=00 (cílový v0), op=d8 (add-int/lit8)
ff02  →  CC=ff (konstanta -1), BB=02 (zdrojový v2 = p0)
```

> **Pokus:** `./build/droidvm tests/data/vmtest.dex --main VmTest --trace 2>&1 | grep fib | head -20` vypíše, jak se instrukce vykonávají.
>
> **Pokus:** Napiš si vlastní třídu, přelož ji (`javac --release 8` + `dx`, viz `tests/regenerate-data.sh`) a rozeber baksmali (`org.smali:baksmali` z Maven Central).

## 5. Interpret

`VM::interpret()` v `interp.cpp` je klasická smyčka **fetch – decode – execute**:

```cpp
for (;;) {
    uint16_t inst = insns[pc];     // fetch
    uint8_t op = inst & 0xff;      // decode
    switch (op) {                  // execute
    case 0x90 ... 0xaf: ...        // aritmetika vAA = vBB op vCC
    case 0x6e: ...                 // invoke-virtual
    }
}
```

Důležité detaily:

- **Registry jsou 64bitové** (`Reg = uint64_t`). int a float leží ve spodních 32 bitech, long a double zaberou celý registr a reference na objekt je ukazatel. V Dalviku zabírá long dvojici registrů. My hodnotu držíme v prvním registru dvojice a druhý jen „rezervujeme“, takže se registry kopírují 1:1 a nic se nerozbije.
- **Java sémantika, ne C++**: dělení `INT_MIN / -1`, posuny (`b & 31`) a převod `NaN → int` jsou v C++ nedefinované. Proto jsou na ně vlastní funkce (`idiv`, `javaConvert`).
- **Volání metody** (`invoke-*`): posbírají se argumenty z registrů, metoda se najde a zavolá se `VM::invoke()`. Ta buď spustí nativní C++ funkci, nebo rekurzivně interpret. Každé volání Javy je tedy jedno volání C++.
  - `invoke-static` a `invoke-direct` volají přesně tu metodu, na kterou instrukce odkazuje,
  - `invoke-virtual` a `invoke-interface` hledají implementaci podle *skutečné* třídy objektu (`Class::dispatch`, s cache). Díky tomu funguje polymorfismus,
  - `invoke-super` začne hledat v rodiči.
- **Výjimky**: Java výjimku nesu jako C++ výjimku `JavaThrow`. Když vyletí uvnitř metody, interpret ji chytí, podívá se do tabulky `tries` (které rozsahy instrukcí chrání který `catch`) a buď skočí na handler, nebo výjimku pošle o patro výš.

## 6. Třídy, objekty a linkování

`vm.cpp`. Třída se načítá líně, až když je poprvé potřeba (`findClass`):

1. **Hledání**: nejdřív mezi již načtenými, pak v DEX souborech aplikace, pak mezi nativními třídami. Když není nikde, vytvoří se **stub** a vypíše varování.
2. **Definice**: nejdřív se načte rodič a rozhraní, pak se vytvoří `Field` a `Method` pro vše, co třída deklaruje.
3. **Linkování** (`link`): každé instanční pole dostane **slot**, tedy index do `Object::fields`. Potomek pokračuje tam, kde skončil rodič, takže pole rodiče mají v potomkovi stejný index a kód rodiče funguje i na potomkovi.
4. **Inicializace** (`ensureInitialized`): při prvním použití (new, statické pole nebo statická metoda) se nastaví počáteční hodnoty statických polí a spustí se `<clinit>` (blok `static { … }`).

Objekt je jen `Class*` + pole slotů + volitelný **NativePeer**, tedy C++ data, která potřebují nativní třídy (text TextView, barva Paint…).

## 7. Garbage collector

Java neumí `free`, takže paměť uklízí GC. My používáme nejjednodušší správný algoritmus, **mark & sweep** (`VM::collectGarbage`):

1. **Mark**: od kořenů (statická pole, internované řetězce, registrované kořeny jako aktivity nebo čekající `Runnable`, a registry všech běžících rámců) projdi všechny dosažitelné objekty a označ je. Odkud jsou odkazy uvnitř objektu, ví třída (`refSlots`, sloty s typem `L…;` nebo `[…`). U pole objektů jsou to prvky a u peerů metoda `trace()`.
2. **Sweep**: všechny neoznačené objekty smaž.

Registry rámců procházíme **konzervativně**: neznáme typ registru, takže každou hodnotu, která vypadá jako adresa známého objektu, bereme jako odkaz. GC spouštíme v hlavní smyčce, když neběží žádný Java kód.

## 8. Nativní knihovna Javy

Skutečný Android má celou standardní knihovnu (libcore) napsanou v Javě. My ji píšeme přímo v C++ (`java_lang.cpp`). Nativní metoda je lambda:

```cpp
string->addNative("charAt", "(I)C", NATIVE {
    auto& s = self(a[0]);           // a[0] = this
    int32_t i = asInt(a[1]);        // a[1] = první argument
    ...
    return s[i];
});
```

Argumenty `a[]` jsou rozložené přesně jako registry: long nebo double zabírá **dva** sloty. Proto má `Math.max(JJ)J` druhý argument v `a[2]`.

## 9. Android framework

Tohle je největší část (`android.cpp`). Používá techniku **HLE (high-level emulation)**: neemulujeme procesor telefonu ani skutečný kód frameworku, ale přímo implementujeme jeho API. Tak to dělají i emulátory konzolí, když místo BIOSu napíšou jeho funkce. Hezký příklad je `AppCompatActivity`: ve skutečnosti je to obrovská knihovna přibalená v APK, ale my ji nadefinujeme nativně jako obyčejnou `Activity`, a protože nativní třídy mají přednost, ta z APK se vůbec nepoužije.

### Životní cyklus aktivity

`AndroidRuntime::createActivity` vytvoří objekt aktivity (`new MainActivity()`) a zavolá `onCreate(null)`, `onStart()`, `onResume()`. `startActivity(intent)` přidá aktivitu na zásobník a `finish()` (nebo Esc) ji odebere a zavolá `onPause`, `onStop` a `onDestroy`.

### Measure → layout → draw

Stejně jako v Androidu se obrazovka kreslí ve třech průchodech stromem View:

1. **Measure**: rodič každému dítěti řekne, kolik má místa, pomocí **MeasureSpec** (int, kde horní 2 bity jsou režim `EXACTLY`/`AT_MOST`/`UNSPECIFIED` a zbytek velikost). Dítě v `onMeasure` spočítá, jak velké chce být, a zavolá `setMeasuredDimension`. Pravidla, jak z `LayoutParams` (`MATCH_PARENT`, `WRAP_CONTENT`, px) a specifikace rodiče vznikne specifikace dítěte, jsou v `childSpec()` a jsou stejná jako `ViewGroup.getChildMeasureSpec()` v Androidu.
2. **Layout**: rodič dětem přidělí pozice (`layout(l, t, r, b)` → `onLayout`). `LinearLayout` je skládá pod sebe nebo vedle sebe a rozdělí zbylé místo podle `weight`.
3. **Draw**: pozadí, pak `onDraw(canvas)`, pak děti.

Klíčový trik: framework volá **virtuální** Java metody `onMeasure`, `onLayout`, `onDraw` a `onTouchEvent`. Vestavěné widgety je implementují nativně. Když tedy aplikace podědí `View` a přepíše `onDraw`, automaticky se zavolá její verze. Přesně tak funguje `BallView` v ukázce.

### Dotyky

Kliknutí myší se převede na `MotionEvent`. `hitPath` najde cestu stromem k nejhlubšímu View pod kurzorem a událost se nabídne nejdřív nejhlubšímu. Kdo na `ACTION_DOWN` vrátí `true`, dostane i další `MOVE` a `UP`. Výchozí `View.onTouchEvent` udělá stav „stisknuto“ a při puštění zavolá `performClick()` → `OnClickListener.onClick()`. `ScrollView` si gesto převezme, když se prst pohne svisle o víc než 8 dp.

### Hlavní smyčka a Handler

Android aplikace běží v jednom hlavním vlákně se smyčkou zpráv (Looper). `Handler.postDelayed(r, 16)` jen zařadí `r` do fronty s časem. Naše smyčka (`main.cpp` → `AndroidRuntime::tick`) spouští callbacky, jejichž čas nadešel, a když něco zavolalo `invalidate()`, překreslí obrazovku. Z toho je animace v `BallView`.

## 10. Softwarový rasterizér

`raster.cpp`. Framebuffer je pole 32bitových pixelů `0xAARRGGBB`, tedy stejný formát jako barvy v Androidu (`Color.RED == 0xFFFF0000`).

- **Vyhlazování přes pokrytí (coverage)**: pro každý pixel spočítáme, jak velkou část pixelu tvar pokrývá (0 až 1), a barvu smícháme s pozadím: `výsledek = barva·α·pokrytí + pozadí·(1 − α·pokrytí)` (*source over*).
- **Signed distance function (SDF)**: kruh, ovál, zaoblený obdélník i čára jsou popsané funkcí „vzdálenost bodu od okraje“ (záporná uvnitř, kladná venku). Pokrytí je pak `clamp(0.5 − vzdálenost)`. Jedna funkce `coverageFill` tak umí všechny tvary a obrys je jen `|vzdálenost| − půlka tloušťky`.
- **Zrychlení dlaždicemi**: plochu procházíme po dlaždicích 8×8. Když je střed dlaždice od okraje dál než její poloviční úhlopříčka, je celá uvnitř (vyplň bez počítání) nebo celá venku (přeskoč). Tím se vykreslování zrychlilo zhruba 2×.
- **Text**: `tools/genfont.py` předem vyrenderuje znaky fontu DejaVu Sans ve velikosti 48 px do `font_data.h` (alfa maska + metriky). Při kreslení se maska zmenší bilineárním vzorkováním.
- **Transformace a ořez**: `save()`, `restore()`, `translate()` a `clipRect()` drží zásobník stavů. Každé View se kreslí posunuté na svou pozici a ořezané na své hranice.

## 11. Hlavní program

`main.cpp` rozbalí APK, načte DEX soubory, přečte manifest a spustí aktivitu. Pak buď otevře SDL okno (myš = prst, kolečko = scroll, Esc = Zpět) a framebuffer každý snímek nahraje jako texturu, nebo v režimu `--headless` běží ve **virtuálním čase** (16 ms na snímek, ale tak rychle, jak to procesor zvládne) a provede naskriptované dotyky. Na tom stojí automatické testy.

## 12. Jak ladit

- `[droidvm] class/method not implemented: …` říká, co aplikace volá a co ještě neumíme. To je tvůj seznam úkolů.
- `--trace` vypisuje každou vykonanou instrukci.
- `ctest --test-dir build --output-on-failure` spustí testy. `vm_matches_jvm` porovná výstup programu na droidvm a na skutečné JVM, takže je to nejlepší způsob, jak najít chybu v interpretu (tak se našla chyba ve znaménku `const/4`).
- Výjimku, kterou aplikace nechytí, vypíšeme jako Android (`FATAL EXCEPTION: main`) i se zásobníkem volání.

## 13. Srovnání se skutečným Androidem

| Android | droidvm |
|---|---|
| ART: DEX se při instalaci a za běhu překládá do strojového kódu (AOT/JIT) | čistý interpret, pomalejší, ale jednoduchý |
| libcore a framework napsané v Javě (miliony řádků) | ~2 600 řádků C++ jen pro nejpoužívanější API |
| víc procesů, Zygote, Binder IPC, system_server | jeden proces, jedno vlákno |
| SurfaceFlinger, GPU, Skia nebo HWUI | vlastní softwarový rasterizér |
| `resources.arsc` + XML layouty | zatím jen UI v kódu (viz cvičení) |

Kdo potřebuje spouštět skutečné aplikace z Google Play, použije `bsdroid`: ten v hypervisoru bhyve spustí celý Android (BlissOS) i s Google Play.

## Kam dál

- Specifikace DEX: https://source.android.com/docs/core/runtime/dex-format
- Instrukce Dalviku: https://source.android.com/docs/core/runtime/dalvik-bytecode
- Formáty instrukcí: https://source.android.com/docs/core/runtime/instruction-formats
- Jak Android kreslí View: https://developer.android.com/guide/topics/ui/how-android-draws
- Pak pokračuj [cvičeními](cviceni.md).
