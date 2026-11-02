# Oracolo differenziale (issue #11)

L'oracolo esegue funzioni (o tratti di codice) dell'originale `gromada.exe` in un emulatore x86
([unicorn](https://www.unicorn-engine.org/)) e ne registra ingressi e uscite come **vettori di
test numerici** (`tests/data/oracle/*.json`). Le funzioni di riferimento C++ pure di
`src/game/reference_math.hpp` devono riprodurli **bit per bit** (`tests/oracle_test.cpp`).
Il motore (issue #8) può chiamare direttamente queste funzioni o essere confrontato con esse.

- I vettori contengono solo numeri (ingressi scelti dal generatore e uscite dell'originale):
  nessun byte dell'eseguibile né dati del gioco. Sono nel repo, quindi la CI non ha bisogno
  dei dati.
- La **rigenerazione** richiede `gromada.exe` (`$GROMADA_DATA/gromada.exe` o `$GROMADA_EXE`).
- Tutti gli indirizzi sono VA dell'eseguibile originale (image base `0x400000`).

## 1. Metodo

`tools/oracle/oracle.py` (classe `Oracle`):

- **Caricamento**: le sezioni del PE (pefile) sono copiate agli indirizzi virtuali, la `.bss`
  azzerata; nessuna rilocazione (l'immagine sta al suo image base). Regioni aggiuntive: heap
  sintetico `0x10000000` (allocatore a puntatore), pila `0x20000000`, area stub `0x30000000`.
- **Import**: ogni slot della IAT punta a un indirizzo trappola; una chiamata a un'API non
  prevista ferma tutto con `UnexpectedCall`. Le API necessarie hanno un gestore Python
  (`stub_import`, es. `timeGetTime`).
- **Stub e registratori**: `stub(addr, callback, popped)` sostituisce una funzione interna
  (hook sul primo byte: il gestore legge gli argomenti dalla pila, imposta `eax` ed esegue il
  `ret n`). `recorder()` registra le chiamate in `oracle.calls`. Così le dipendenze (es.
  `ChangeAction`, `CheckMove`, la `Action` virtuale) vengono **osservate** invece che eseguite,
  e la funzione sotto test resta isolata. Le vtable sintetiche (1 slot) puntano a stub
  registratori.
- **Chiamate**: `call(addr, args, conv, this)` costruisce la pila, mette un indirizzo di ritorno
  magico ed esegue fino al ritorno; controlla che il chiamato abbia tolto dalla pila esattamente
  i byte previsti dalla convenzione (un prototipo sbagliato viene scoperto subito).
- **Tratti di codice**: `run(begin, ends, regs, frame_init)` esegue un intervallo di una funzione
  più grande (es. la fisica dentro `SPRITE::Tact`) con i registri che il codice si aspetta in quel
  punto e un frame di variabili locali riempito con `0xCC` (le locali non scritte si riconoscono).
- **Eccezioni CPU**: una divisione per zero (`#DE`) produce `CpuFault`; i vettori la registrano come
  `crash = 1`. Dopo un'eccezione viene ripristinato un contesto CPU pulito (altrimenti QEMU
  trasformerebbe la successiva in double fault).
- **Strutture sintetiche** (`tools/oracle/structs.py`): tabella dei vid (`[0x42df57]`), sprite,
  vtable, griglia spaziale `0x431ab0` con celle costruite a mano, con gli offset di
  `docs/re/engine.md` §2. I campi derivati dei vid sono calcolati come il caricatore (verificato
  dalla famiglia `vid_derived`).
- **Controlli di robustezza**:
  - i generatori usano un `random.Random` con seme fisso per famiglia: due rigenerazioni danno
    file identici;
  - i registri non impostati valgono `0xDEADBEEF`; rigenerando con `ORACLE_GARBAGE=0` i vettori
    sono **identici**, quindi nessun tratto emulato dipende da registri non inizializzati;
  - ogni generatore contiene `assert` sulle chiamate registrate (numero, argomenti attesi).

## 2. Funzioni coperte

| famiglia (`tests/data/oracle/`) | codice originale | convenzione | casi | note |
|---|---|---|---|---|
| `rand` | `srand` `0x41c7ab`, `rand` `0x41c7b5` | cdecl | 217 semi × 48 uscite | semi limite (0, 1, `0x8f`, seme di `demo1`, `0xffffffff`...) |
| `random_builtin` | builtin `Random(max)` (id 90, `0x4130a4`) via VM | VM | 732 | `max` limite, `-1` = crash |
| `direction` | `GetDirection` `0x40b030` | cdecl (4 × int32) | 3246 | tutte le direzioni a 15 raggi (1..3000), assi, vettore nullo, s16 e int32 interi (overflow) |
| `tables` | tabelle `.data` `0x428e14`/`0x429214`/`0x429e18`; tabelle iniziali di `0x40cf70` | — | 256 × 5 | seno, −coseno, CORDIC, prospettiva 2/3, cotangente |
| `rotation_tables` | `0x40cf70` → `0x431bf4`/`0x435bf4` | cdecl | 64 righe × 512 | passo di rotazione sinistra/destra per ogni `dirs` |
| `map_scale_tables` | ciclo `0x414f36..0x414f8f` del caricamento mappa | intervallo | 1024 | 4 coppie `scaleX/scaleY` (anche negative) |
| `vid_derived` | caricatore vid `0x4066ae..0x406717` | intervallo | 460 | `dirs = 0` = crash |
| `distance_sprite` | `Distance(sprite)` `0x40aff0` | thiscall, `ret 4` | 1600 | s16 ai limiti |
| `distance_point` | `Distance(x, y)` `0x40afc0` | thiscall, `ret 8` | 1515 | punto int32 (overflow) |
| `sizeto` | builtin `SizeTo` (id 65, `0x412bcc`) via VM | VM | 800 | unità nulla, x/y oltre s16 |
| `physics` | `SPRITE::Tact` `0x409cae..0x40a116` | intervallo (`esi`=sprite, `ecx`=vid, `ebp`=0) | 2000 | gravità, accelerazione/frenata (anche 9999), quota con e senza gravità, atterraggio esatto, vento, velocità negative e 9999 (raggio), dt 0..2³²−1 (orologio che torna indietro), bordi mappa, blocco, attraversamento di `go`, cambio di cella |
| `commands` | `SPRITE::Tact` `0x40a116..0x40a201` | intervallo (+ locale `step`) | 2000 | pop dei comandi, `act_submove`, `act_cyclecommand`, liberazione, `act_nextcommand` |
| `set_destination` | `SetDestination` `0x409770` | thiscall, `ret 0xc` | 1200 | `GetDirection` reale; distanza 0, tiri balistici, argomenti fuori s16 |
| `turn_to` | `TurnTo` `0x40b0b0` | thiscall, `ret 4` | 2500 | `dirs` 1..64, `dirStep` incoerente, bersaglio int32 (256+d, negativi) |
| `change_action` | `ChangeAction` `0x40b3e0` + `SetAnimation` `0x40b330` | thiscall, `ret 8` | 1000 | figlio collegato, animazioni mancanti, somme a 16 bit che traboccano |
| `check_move` | `CheckMove` `0x40a5c0` + griglia `0x40d6d0`/`0x40d7a0` | thiscall, `ret 0xc` | 1200 | terreno, maschere, quota, `NoCollision`, `CrushOnCollision`, oggetti in morte, z < 0, celle ai bordi |
| `damage` | `Action` materiale `0x412580`, `act_damage` (43) | thiscall, `ret 0xc` | 2000 | danno negativo, oltre gli hp, `maxHP` 0/1/255 |
| `area_damage` | `Action` materiale `0x412580`, `act_death` (15) | thiscall, `ret 0xc` | 800 | raggio, dimensioni, quota, griglia |
| `vm_arith` | interprete `0x41a790` | VM (`PUSHI a; PUSHI b; OP`) | 5856 | 21 opcode × 16×16 valori limite + casuali |
| `clock` | `SVGA::BeginFrame` `0x401fb0..0x401feb` | intervallo (`timeGetTime` stub) | 600 | limite 71 ms, wraparound di `timeGetTime` |
| `tick_schedule` | `World::Tact` `0x40bc35..0x40bc9d` | intervallo (`edi`=sprite) | 1200 | `Randomized`, `SkipUpdate`, dt "negativi" |
| `sprite_timer` | `SPRITE::Tact` `0x409b39..0x409b76` | intervallo | 600 | timer a 16 bit |

Formato dei file: `{"family", "function", "columns", "notes", "cases": [[...], ...]}`, un caso per
riga; `tables` ha invece `"arrays"`. Le famiglie con oggetti in numero variabile (`check_move`,
`area_damage`, `commands`) hanno `"objectColumns"`: ogni caso è formato dalle colonne fisse
seguite da `nObjects` gruppi di `objectColumns`. Dimensione totale ≈ 1,9 MB.

### Convenzioni di chiamata trovate

- **cdecl**: `rand`/`srand`, `GetDirection` (4 int32, ritorno in `eax` + distanza nella globale
  `0x439d4c`), l'inizializzazione delle tabelle `0x40cf70`.
- **thiscall** MSVC (`this` in `ecx`, il chiamato toglie gli argomenti): tutti i metodi dello
  sprite (`Distance` `ret 4`/`ret 8`, `SetDestination`/`CheckMove`/`FirstObstacleOnLine`
  `ret 0xc`, `TurnTo` `ret 4`, `ChangeAction` `ret 8`, `Action` virtuale `ret 0xc`), la griglia
  (`Query` `0x40d6d0` `ret 0x10`, `Next` `0x40d7a0`, celle `0x40d290`/`0x40d320` `ret 4`),
  l'interprete (`0x41a790`, `this` = oggetto LOGIC).
- **Valori di ritorno impacchettati**: `FirstObstacleOnLine` `0x40a770` restituisce `x | y << 16`
  (0 = nessun ostacolo).
- Le funzioni a 16 bit leggono gli argomenti int32 dalla pila e li troncano solo quando li
  memorizzano (es. `SetDestination` passa a `GetDirection` gli int32 interi).

## 3. Discrepanze con `docs/re/engine.md` e `tools/re/engine_reference.py`

Verificate in emulazione (i vettori le contengono e i test C++ le riproducono). In grassetto le
più importanti per il motore.

1. **Esecuzione dei comandi in `SPRITE::Tact`** (§2.4, §3.3): la condizione è
   `frame > lastFrame || sprite.action < 6` (**azione corrente dello sprite**, `cmp byte [esi+0x13], 6`),
   non `vid.class < 6`. Quindi gli sprite fermi, in moto o in rotazione (azioni 0..5) valutano i
   comandi a ogni tick, gli altri solo a fine animazione.
2. **Condizione di `act_nextcommand`** (§3.3): senza comando da prelevare, `act_nextcommand`
   parte se `(state & 3) == 3`, **oppure se l'azione non è `go`/`l_rotate`/`r_rotate` (2/4/5)**,
   oppure se `step >= vid.rotationPeriod`. Non dipende dalla classe. `act_cyclecommand(n)`
   imposta `count = n` ed esegue subito `e[n]` (il conteggio resta `n`).
3. **Vento e passo** (§4.2): per i vid con flag `Wind` il passo `step` (usato da 2.) è
   `|windStrength·dt| >> 10 + stepAccum`, non `|speed·dt| >> 10`: l'originale riusa il registro
   del prodotto. Con vento nullo il passo resta `stepAccum` anche se lo sprite si muove.
4. **Distanza di `GetDirection` oltre 2047 px** (§4.1): `(x·636750) >> 20` è calcolato a 32 bit;
   con il guadagno CORDIC (≈1,647) trabocca per distanze ≥ ~2048 px e il risultato si riavvolge in
   [−2048, 2048) (es. distanza reale 2500 → circa −1596). Riguarda tutto ciò che usa `g_dist`:
   `SetDestination` (distanza 0 per caso → ritorno anticipato; `vz` balistico), `act_jump`.
   Verificato su 1077 casi s16 con distanza negativa. `engine_reference.py` lo riproduce (`to_s32`)
   ma il documento parla di distanza euclidea.
5. `GetDirection` restituisce `angolo >> 1` **non mascherato** (−5..261); i chiamanti salvano il
   byte basso. `engine_reference.get_direction` applica `& 0xff` (uguale dopo il troncamento a byte;
   per argomenti int32 che traboccano il riferimento Python differisce perché non riavvolge le
   differenze: 244 casi, tutti fuori dal range s16).
6. **Quota senza gravità** (§4.2): se `z == goZ` (con `vz != 0`) azzera `vz` senza muovere; salendo,
   `goZ − z <= dz` aggancia `z = goZ` e azzera `vz`; scendendo l'aggancio avviene solo se
   `dz < goZ − z` (strettamente): **un arrivo esatto dall'alto lascia `vz` invariato** (42 casi nei
   vettori). Toccando terra (`z + dz < 0` con `z >= 0`) viene chiamato `path_block(0, 0)` e `z` non
   cambia in quel tick.
7. Velocità (§4.2): accelerazione `speed = (u16)(speed + ((acc·dt) >>> 10))` e frenata
   `(u16)(speed − ((acc·dt) >>> 9))` con prodotti **senza segno a 32 bit** e risultato a 16 bit; in
   frenata il controllo `< 0` è sul valore a 16 bit, quindi per `acc·dt` enormi (dt dopo una lunga
   pausa) la velocità può riavvolgersi a un valore positivo. Gravità: `vz −= (dt·174) >>> 10`
   (a 16 bit). `engine_reference.integrate_speed` differisce solo in questi casi di overflow
   (43/1941).
8. **`CheckMove`** (§6): la categoria è confrontata **per uguaglianza** (`== 4`, `== 8`, `== 0x10`),
   non come maschera: un vid con categoria composta (es. `0x0c`) non collide mai. Il test "terreno"
   (`o.category & 1`, solo a quota 0) precede il controllo `s != this`; `CheckMove` restituisce
   `this` se `z < 0`.
9. **Danno ad area** (§7.5): il test orizzontale è `|dx| < deathDamageRadius + halfX(esploso) +
   o.halfX` (il documento omette la metà dimensione di chi esplode; idem per y). Il raggio è letto
   con segno a 16 bit (`movsx`: 65535 vale −1). Prima del ciclo l'esploso ha `hp = 0`. Lo sprite
   stesso viene saltato; servono `o.maxHP != 0` e `|dz| < o.absSizeZ + 20`.
10. **`act_damage`** (§3.4): se `n >= hp` (con segno) → `ChangeAction(death)` **senza** modificare
    `hp`; altrimenti `hp = (u8)(hp − n)` (un danno negativo cura, con riavvolgimento a 8 bit).
    `ChangeAction(wound)` solo quando `hp` **scende** da sopra `maxHP/2` a ≤ `maxHP/2`; quando risale
    sopra metà viene chiamato `0x40c0e0(vid.childNvid[13])` (rimozione degli effetti di ferita).
11. **`TurnTo`** (§4.6): se lo sprite sta già ruotando nel verso opposto (azione 5 quando serve
    sinistra, 4 quando serve destra) chiama `ChangeAction(go, −1)` invece di invertire. Restituisce
    1 (e imposta la direzione esatta) anche quando i due indici quantizzati coincidono; il primo
    confronto è fra l'argomento int32 e il byte di direzione (256+d non è "uguale" ma dà lo stesso
    risultato). La tabella di rotazione per `dirs = 1` contiene i valori di 32 direzioni (mai letta).
    Le tabelle esistono solo fino a `dirs = 64`.
12. `SetDestination` (§4.3): con `g_dist == 0` ritorna subito dopo `state &= ~3` (go, frazioni e
    `vz` restano com'erano); il termine balistico è `(174·dist / speedX) >> 1` (shift aritmetico,
    non `/2`); la categoria Cannon è confrontata con `== 0x10`; `z` è troncato a s16.
13. `ChangeAction` (§3.2): usa il byte basso di `act`; `state &= ~0x200` sempre; il figlio segue
    `death` sempre e `open/close/stand_open` solo se il **suo** vid ha quell'animazione; il ritorno
    anticipato confronta il `dir` int32 con il byte di direzione; la correzione
    `frame = lastFrame` per `death/wound/birth` senza animazione vale anche nel ramo `stand_open`.
    In `SetAnimation` le somme dei fotogrammi sono a 16 bit.
14. VM (runtime.md §2.4): oltre a `x % 0`, anche **`INT_MIN / −1` e `INT_MIN % −1` mandano in crash**
    l'originale (#DE). `Random(−1)` va in crash dopo aver consumato un `rand()`. `SizeTo(0, ...)`
    restituisce 60000; `x`/`y` sono troncati a s16.
15. Tick (§1.4): il confronto `curclock − lastTact >= period` è senza segno (un `lastTact` nel futuro
    fa scattare subito il tick); il jitter `Randomized` consuma un `rand()` anche se il tick non
    scatta; `SkipUpdate` non consuma `rand()`.
16. Caricatore dei vid: `rotationPeriod = (s8)(rot·6 / dirs)` con segno; `0x127 = (u8)(256/dirs)`
    (0 per `dirs = 1`); `dirs = 0` divide per zero.

`tools/oracle/check_engine_reference.py` confronta automaticamente `engine_reference.py` con i
vettori (tabelle, direzione, distanza, orologio, velocità, moto, periodo dei tick).

## 4. API di `src/game/reference_math.hpp`

Namespace `gromada::game::ref`, header-only, solo `<array>`, `<cstdint>`, `<optional>`, `<span>`;
tutte le funzioni sono `constexpr` e non dipendono dalle strutture del motore.

| gruppo | funzioni |
|---|---|
| aritmetica x86 | `add32`, `sub32`, `mul32`, `neg32`, `abs32`, `lo16`, `lo8`, `idivFaults` |
| tabelle | `kSin`, `kNegCos`, `kCordicAngles`, `perspectiveScaled`, `startupScaled`, `cotEntry`, `rotationStep(dirs, dir, right)` |
| vid | `deriveVid` |
| RNG | `MsvcRand{state}.seed/next`, `scriptRandom` |
| direzione e distanze | `getDirection` → `{direction, distance}`, `gameDistance`, `gameDistanceToPoint`, `sizeTo` |
| tempo | `clockAdvance`, `tickPeriod`, `tickDue`, `timerCountdown` |
| VM | `vmBinary(op, a, b)`, `vmUnary(op, a)` (`std::nullopt` = crash), costanti `VmOp` |
| animazione | `AnimVid`, `AnimSprite`, `setAnimation`, `changeAction` (con figlio opzionale) |
| movimento | `turnTo` → `TurnResult`, `setDestination` → `DestResult`, `physicsStep(sprite, vid, world, action, blocked, obstacle)` → `PhysicsResult` |
| comandi | `commandStep` → `CommandStepResult` |
| griglia e collisioni | `GridSpec`, `gridQuery`, `checkMoveQuery`, `checkMove(self, x, y, z, candidates, crush)` |
| danni | `applyDamage`, `Exploder`, `DamageTarget`, `areaDamageQuery`, `areaDamage(exploder, candidates, hit)` |

Le funzioni che nell'originale chiamano altro codice ricevono **callback** (`action`, `blocked`,
`obstacle`, `crush`, `hit`) o restituiscono la richiesta (`changeAct`, `kind/act`), così il motore
le collega alle proprie strutture. `physicsStep` rilegge lo sprite dopo ogni callback, come
l'originale. Le collisioni ricevono i candidati già nell'ordine di visita della griglia
(`gridQuery`: celle allargate di una, righe y esterne, x interne; in una cella l'ordine di
inserimento).

## 5. Limiti

- Le partite intere (ordine delle chiamate fra sprite, RNG su più frame, audio) sono verificate
  dall'oracolo a livello di partita sulle demo: `docs/trace-oracle.md` (issue #13).
- Prestazioni: `MAX_INSNS` passa a unicorn il parametro `count`, che aggiunge un hook a ogni
  istruzione (circa 50 volte più lento); per esecuzioni lunghe usare `count=0` come
  `trace_game.py`.

- Si testano funzioni e tratti isolati, non partite intere: l'ordine delle chiamate fra sprite e
  il consumo dell'RNG su più frame restano da verificare con le demo (issue #3/#8).
- Le dipendenze sono sostituite da registratori: per esempio la fisica non esegue la vera
  `CheckMove` (coperta a parte) né le reazioni di `path_block`/`path_limit` delle classi.
- Precondizioni non coperte (l'originale andrebbe in errore o leggerebbe memoria estranea):
  `action >= 16` in `SetAnimation` (errore fatale), `dirs > 64` in `TurnTo`, `scaleX = 0`, overflow
  delle divisioni balistiche di `SetDestination` (`INT_MIN / −1`, praticamente irraggiungibile).
- `ChangeAction` è testata con vid sintetici (stessa logica dei vid reali: legge solo lunghezze,
  `dirs` e `0x127`); niente dati del gioco nei vettori.
- Non coperte (ancora): `FirstObstacleOnLine` `0x40a770`, `AskPlace` `0x40a430`, `FindTarget`
  `0x40c970`, `Aim`/`Fire`, `CreateChildren`, le `Action` delle classi; si aggiungono con lo stesso
  schema.

## 6. Rigenerare i vettori

```sh
python3 -m venv /root/tools/oracle-venv && /root/tools/oracle-venv/bin/pip install unicorn pefile
GROMADA_DATA=/percorso/del/gioco /root/tools/oracle-venv/bin/python -I tools/oracle/generate.py          # tutte
/root/tools/oracle-venv/bin/python -I tools/oracle/generate.py gen_physics gen_vm                        # alcune
ORACLE_GARBAGE=0 /root/tools/oracle-venv/bin/python -I tools/oracle/generate.py && git diff --stat        # nessuna differenza attesa
python3 -I tools/oracle/check_engine_reference.py                                                        # confronto con engine_reference.py
```

Versioni usate: unicorn 2.1.4, pefile, Python 3.13. Poi `cmake --build build && ctest -R oracle`.

## 7. Aggiungere una funzione

1. Trovare indirizzo, convenzione e dipendenze (`tools/re/gromada_re.py "dis ADDR"`).
2. Creare `tools/oracle/gen_<nome>.py` con `generate(o)` che restituisce una lista di
   `common.family(...)`:
   ```python
   from common import family, rng
   from structs import World
   def generate(o, n=2000):
       r, w = rng("nome"), World(o)
       o.stub(0x40b3e0, o.recorder("ca", 2), popped=8)        # dipendenze osservate
       cases = []
       for _ in range(n):
           w.reset(); o.calls.clear()
           w.set_vid(1, {"speedX": r.randrange(300)})
           s = w.sprite({"x": r.randrange(4000), "nvid": 1})
           ret = o.call(0x40xxxx, [arg], conv="thiscall", this=s)
           cases.append([arg, ret, w.get(s, "x")])
       return [family("nome", "Descrizione 0x40xxxx (thiscall)", ["arg", "ret", "x'"], cases)]
   ```
   Per un tratto di funzione: `o.run(inizio, [fine, ...], regs={...}, frame_init={offset: valore})`.
3. Aggiungere il modulo a `MODULES` in `generate.py`, rigenerare e controllare dimensione
   (totale < 2 MB) e copertura dei rami.
4. Scrivere la funzione pura in `reference_math.hpp` e il controllo in `tests/oracle_test.cpp`
   (`forCases("nome", ...)`).
