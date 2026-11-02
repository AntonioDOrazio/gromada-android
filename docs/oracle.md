# Oracolo differenziale (issue #11)

L'oracolo esegue funzioni (o tratti di codice) dell'originale `gromada.exe` in un emulatore x86
([unicorn](https://www.unicorn-engine.org/)) e ne registra ingressi e uscite come **vettori di
test numerici** (`tests/data/oracle/*.json`). Le funzioni di riferimento C++ pure di
`src/game/reference_math.hpp` devono riprodurli **bit per bit** (`tests/oracle_test.cpp`).
Dalla issue #12 anche il **codice del motore** (`World`, `GameEngine`, l'interprete) è verificato
direttamente su tutti i vettori (`tests/engine_oracle_test.cpp`, §9).

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
- **Vtable reali e allocatore** (issue #12): per le `Action` delle classi gli sprite sintetici
  hanno la vtable vera della classe (`tools/oracle/units.py`, `VTABLE`), così le chiamate
  virtuali interne eseguono il codice originale; `malloc`/`realloc`/`free` della CRT
  (`0x41ca48`/`0x41c928`/`0x41c881`) sono sostituiti da un allocatore sull'heap sintetico (pile dei
  comandi). Un accesso a memoria non valida dell'originale (es. `0x40c0e0` oltre la fine della
  catena dei figli) è registrato come `crash = 1`.
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
| `sound_edge`, `sound_random` | oggetto audio: `0x408dd0`, `0x408f20`, `0x408910`, `0x409550`, `0x408d80`, `0x408c50` | thiscall | 255 + 1100 sequenze | famiglia "suono" (§8): chiamate DirectSound registrate |
| `sound_table` | `0x407640` + `0x408730` su `fw.res` | thiscall | 48 descrittori | solo con i dati; tipo, formato, CRC32 |
| `ask_place` | `AskPlace` `0x40a430` / `AskPlaceVid` `0x40a2d0` + griglia | thiscall `ret 0xc` / cdecl | 1200 | come `check_move`; `AskPlaceVid` incontra lo sprite stesso nella griglia |
| `first_obstacle` | `FirstObstacleOnLine` `0x40a770`, `AskPlace` registrata | thiscall, `ret 0xc` | 2000 | sequenza delle sonde (numero, somme, ultima), blocco alla sonda k, linee lunghe/corte/verticali, posizioni negative, punti int32 |
| `first_obstacle_grid` | `FirstObstacleOnLine` con `AskPlace` e griglia reali | thiscall | 800 | oggetti lungo la linea, quota crescente |
| `find_target` | `FindTarget` `0x40c970` | thiscall | 1500 | lista unità con lo sprite stesso in posizione varia, eserciti (2 = neutrale), invisibili, raggio, armi direzionali e antiaeree (`rand`) |
| `aim_fire` | `Fire` `0x40e6b0` / `Aim` `0x40ada0`, `TurnTo` reale, `ChangeAction` registrata | thiscall, `ret 0xc` | 1500 | torretta collegata o no, flag 1/4/8, portata, `g_dist` stantio, timer |
| `create_children` | `CreateChildren` `0x40a950`; `CreateUnit`, `AskPlaceVid`, `SetDestination`, `act_child` registrate | thiscall | 1200 | offset ruotati e casuali, direzione casuale, dispersione, figli Vehicle, creazioni fallite |
| `action_projectile` | `Action` Projectile `0x40f710` | thiscall, `ret 0xc` | 600 | azioni 15 31 38 39 40 43 45 46 57 65 |
| `action_vehicle` | `Action` Vehicle `0x40ea20` | thiscall, `ret 0xc` | 1400 | 24 azioni, `act_nextcommand` (waypoint, sterzata, bersagli, fuoco, radar) |
| `action_kassandra` | `Action` Kassandra `0x411a60` | thiscall, `ret 0xc` | 1000 | input del giocatore (`0x43d045`), salto, munizioni, `act_coor_attack` con le teste `0x58..0x5c` |
| `action_building` | `Action` Building `0x410e10` | thiscall, `ret 0xc` | 800 | produzione (timer, `CreateUnit` che fallisce), sollevamento del carico, `act_buildunit` |

Formato dei file: `{"family", "function", "columns", "notes", "cases": [[...], ...]}`, un caso per
riga; `tables` ha invece `"arrays"`. Le famiglie con oggetti in numero variabile (`check_move`,
`area_damage`, `commands`) hanno `"objectColumns"`: ogni caso è formato dalle colonne fisse
seguite da `nObjects` gruppi di `objectColumns` (anche `ask_place`, `first_obstacle_grid`,
`find_target`, `create_children`). Dimensione totale ≈ 6 MB: ≈ 4,5 MB per le famiglie del motore
(le `action_*` hanno più di 100 colonne: stato completo di vid, arma, sprite, bersaglio e torretta
prima e dopo) e ≈ 1,5 MB per la famiglia "suono" (righe di lunghezza variabile, formato in §8).

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

Trovate con le famiglie della issue #12 (tutte corrette nel motore salvo dove indicato):

17. **`FirstObstacleOnLine` restituisce `(y << 16) + x` come somma a 32 bit** (`0x40a91b`): una x
    negativa prende in prestito da y (chi decodifica legge `y − 1`). Le sonde partono dalla terza
    (`t > 16`), a passi di 16 px sull'asse maggiore; l'ultima prova è sempre il punto finale.
18. **Antiaereo = `weapon.targetCategory & 8`**, non `flags & 8`: `act_coor_attack` di Vehicle
    (`0x40eadb`) e Kassandra (`0x411c41`) e la torretta del giocatore (`0x416be1`) mirano 80 px più in
    alto se l'arma colpisce gli aerei. `flags & 8` è usato solo da `FindTarget` (la preferenza 1/3).
    `docs/re/engine.md` §7.1 attribuisce il "+80" al flag.
19. La torretta del giocatore (`Player::Tact`) usa l'arma del **vid della testa** per il flag 4 e
    per l'antiaereo; l'arma del flagman solo per filtrare l'unità sotto il cursore.
20. Projectile `act_nextcommand` (`0x40f7f6`): se `TurnTo` avvia una rotazione la funzione
    **ritorna** (niente `act_go`).
21. Projectile `act_attack` (`0x40f917`): la direzione è calcolata verso `(v1, v2)`, cioè verso
    l'**indirizzo** del bersaglio (un bug dell'originale: un puntatore di heap, quindi circa est);
    la destinazione invece è giusta.
22. Kassandra `act_damage` letale (`0x41211a`): incrementa un contatore statistico nel record del
    giocatore (`0x439fbd + army·0x255 + …`, indicizzato dal gruppo di palette di chi spara o dagli
    nvid `0x70`/`0x78`) prima del danno materiale. Il parametro `v2` (lo sprite che spara) è
    dereferenziato: non può essere nullo.
23. `0x40c0e0` (rimozione degli effetti di ferita) dereferenzia la fine della catena dei figli:
    l'originale va in errore se il vid `childNvid[13]` non è agganciato (3+2 casi `crash` nelle
    famiglie `action_*`).
24. `FindTarget` con arma direzionale chiama `GetDirection`, che **riscrive `g_dist`** (`0x439d4c`):
    lo leggono poi `Aim` e altri (il motore usava la versione pura senza effetti).
25. `act_coor_attack` della Kassandra legge la testa senza controllo di nullità e indicizza le
    munizioni con `nvid − 0x58`/`− 0xca`/`− 0xcf` senza controllo dei limiti.

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

- Si testano funzioni e tratti isolati, non partite intere: l'ordine delle chiamate fra sprite e
  il consumo dell'RNG su più frame restano da verificare con le demo (issue #3/#8).
- Le dipendenze sono sostituite da registratori: per esempio la fisica non esegue la vera
  `CheckMove` (coperta a parte) né le reazioni di `path_block`/`path_limit` delle classi.
- Precondizioni non coperte (l'originale andrebbe in errore o leggerebbe memoria estranea):
  `action >= 16` in `SetAnimation` (errore fatale), `dirs > 64` in `TurnTo`, `scaleX = 0`, overflow
  delle divisioni balistiche di `SetDestination` (`INT_MIN / −1`, praticamente irraggiungibile).
- `ChangeAction` è testata con vid sintetici (stessa logica dei vid reali: legge solo lunghezze,
  `dirs` e `0x127`); niente dati del gioco nei vettori.
- Non coperte (ancora): `CreateUnit`/`Init`, le `Action` di Avia, Mine, missili, Bonus, Effect2,
  `Player::Tact` (verificato solo con un test di regressione), il disegno. Nelle `action_*`
  `CreateUnit` restituisce sempre 0 (nessuna unità creata), il giocatore 0 non ha unità sotto il
  cursore e la mappa non è in rete.

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

## 8. Famiglia "suono" (issue #18)

`tools/oracle/gen_sound.py` esegue le funzioni **non modificate** dell'oggetto audio (`0x42e0c8`,
`docs/re/runtime.md` §9.1) su un oggetto sintetico costruito dal costruttore originale `0x4080c0`
(come la globale in `.bss`), con una tabella dei descrittori sintetica (N effetti, tipi 0, 1, 40,
50, 90, 100 e byte casuali, dati PCM casuali) e un **DirectSound finto**:

- `IDirectSound` (vtable a stub): `CreateSoundBuffer` `+0x0c`, `DuplicateSoundBuffer` `+0x14`;
  `IDirectSoundBuffer`: `Release` `+0x08`, `GetStatus` `+0x24`, `Lock` `+0x2c`, `Play` `+0x30`,
  `SetCurrentPosition` `+0x34`, `SetVolume` `+0x3c`, `SetPan` `+0x40`, `Stop` `+0x48`, `Unlock` `+0x4c`.
  Ogni altro slot → `UnexpectedCall`. Metodi stdcall (COM): l'harness controlla i byte tolti dalla
  pila. Verificati: `DSBUFFERDESC` (dimensione 0x14, flag `0xe2` = STATIC|CTRLFREQUENCY|CTRLPAN|
  CTRLVOLUME, byte e formato del descrittore), il riempimento `Lock`/`memcpy`/`Unlock` di `0x408b30`
  (contenuto identico ai dati del descrittore), nessun buffer perso o orfano dopo ogni chiamata.
- Stato simulato: un buffer suona da `Play` fino a `Stop`/`Release` o a un evento **FINISH**
  scelto dal generatore (solo buffer non ciclici: il suono è arrivato in fondo). `GetStatus`
  restituisce `DSBSTATUS_PLAYING | DSBSTATUS_LOOPING`. `DuplicateSoundBuffer(NULL)` restituisce
  `DSERR_INVALIDPARAM` come il vero DirectSound (`0x408910` la chiama quando il canale scelto
  precede quello da duplicare: prova anche il proprio buffer, ancora nullo).
- Stub deterministici: orologio `curclock` `0x42bf94` scritto prima di ogni Tact; per `PlayWAV`
  `fopen`/`fclose`/`free` e il caricatore WAV `0x408570` (riempie il descrittore N come un WAV
  valido, oppure lo lascia vuoto, oppure `fopen` fallisce); il log `0x407fb0` è contato.

Ogni riga è una **sequenza di eventi** (interi; formato completo nel campo `notes`):
`TACT(clock)`, `REQUEST(nsfx, pan, att)`, `STOPALL`, `STOPONE(nsfx)`, `PLAYWAV(file, esito, ritorno)`,
`FINISH(canale)`; per ognuno le operazioni DirectSound prodotte (`GetStatus` con il risultato,
`SetPan`, `SetVolume`, `Play(flag)`, `Stop`, `LOAD` = Create+riempimento o Duplicate dal buffer di
un altro canale, `Release`, `SetCurrentPosition`), identificate dal **canale** che possiede il
buffer. In fondo lo stato dei canali (attivo, buffer, nsfx, istante) e della coda.

- `sound_random` (1100): N da 2 a 45, effetti "caldi" (fusioni, duplicati, riuso), nsfx invalidi
  (0, −1, N senza WAV, N+1, int32), pan e attenuazioni limite (±10000/±10001, −2000/−2001,
  positive, int32), passi di orologio (16/17/33/39 ms, 49/50/51, 499/500/501, grandi, riavvolgimento
  a 32 bit), da 0 a 40 richieste per frame (coda piena), StopSFX/StopSFX(n)/PlayWAV, Tact doppi.
- `sound_edge` (255): loop tenuti vivi con periodi 16..520 ms e scadenza a 499/500/501/502 ms,
  guardia dei 50 ms (one-shot, ciclici, non posizionali, finiti o no), più richieste che canali
  liberi (parità: vince l'ultima), attenuazioni positive, coda piena (one-shot e ciclici), fusioni
  con pan fuori limite, StopSFX/StopSFX(n)/StopMusic e ripresa, PlayWAV (stesso file, altro file,
  file mancante o non valido), più di 8 canali occupati, orologio che si riavvolge o torna indietro.
- `sound_table` (solo con `fw.res`): `0x407640` apre il `.res` e `0x408730` carica la tabella con il
  vero `0x408570`; `fopen`/`fread`/`fseek`/`ftell`/`malloc` sono sostituite da Python sul file
  reale. Per ogni descrittore: tipo, byte, campi `WAVEFORMATEX`, CRC32 dei dati (solo numeri).

`tests/sound_oracle_test.cpp` rigioca ogni sequenza su `game::SoundChannels` con un `SoundSink`
che simula lo stesso dispositivo e registra le stesse operazioni: devono coincidere **esattamente**
evento per evento (anche gli argomenti interi di pan e volume), così come lo stato finale. Con
`GROMADA_DATA` confronta anche `GameResources::sounds()` e `effectSound()` con `sound_table`
(N = 47, 47/47 suoni identici per tipo, dimensione, frequenza e CRC32).

Controlli fatti sul test: le mutazioni di guardia (50→49), scadenza (500→501), parità del
"più debole", scadenza solo dei canali attivi e partenza dall'elemento 0 vengono tutte rilevate
(da 29 a 338 sequenze diverse). `ORACLE_GARBAGE=0` dà vettori identici.

### Risultati

- Il vecchio scheduler (b456dcd), rigiocato sugli stessi vettori a livello di ciò che si sente,
  differiva in 582 sequenze su 1355 per due motivi raggiungibili nel gioco: **ripresa dopo `Stop`**
  (un motore che riparte sullo stesso canale continua da dove era stato fermato, non ricomincia;
  idem un effetto interrotto da `StopSFX()`) e lo **slot `PlayWAV`** (mai accodato, non ciclico);
  più differenze solo con attenuazioni positive (mai prodotte dal gioco: aritmetica a 32 bit della
  fusione, ciclo di scarto che parte dall'elemento 0).
- Sound 0 di `fw.res` ha tipo 1 (non ciclico): i canali mai usati, che il costruttore lascia con
  nsfx 0, non vengono mai fermati dal passo 3 del Tact (con tipo 0 l'originale chiamerebbe `Stop`
  su un buffer nullo).
- `SetCurrentPosition` non viene mai chiamata: il riavvio di un buffer fermato riprende dalla
  posizione corrente.

Limiti: `0x408730` è verificata solo su `fw.res`; i casi in cui l'originale andrebbe in crash
(creazione del buffer fallita, dati del descrittore vuoti, `#DE` nella fusione con
`INT_MIN / −1`) sono esclusi; l'audio disattivato (`+0x2a4`) fa uscire subito tutte le funzioni e
non è modellato. L'ordine delle richieste fra sprite in una partita è verificato dall'oracolo di
partita (#13), non qui.

## 9. Il motore sui vettori (issue #12)

`tests/engine_oracle_test.cpp` (senza dati del gioco, nella CI) esegue il **codice del motore** su
tutte le 33 famiglie: `World` costruito da vid sintetici (`World{vids, weapons}` + `loadEmpty`), gli
sprite creati con `createUnit` e poi riempiti con i campi del caso, la griglia con celle di 32 px
(un vid fittizio `PresentOnGrid` di 64 px fissa la dimensione della cella), `GameEngine::callBuiltin`
per `SizeTo`/`Random`, l'interprete di `src/script` per `vm_arith`. Lettore dei vettori condiviso:
`tests/oracle_vectors.hpp`.

Dove l'oracolo registra un collaboratore invece di eseguirlo, il motore offre punti di aggancio
minimi, senza effetti nel gioco:

| aggancio | usato da |
|---|---|
| `World::actionHook`, `World::changeActionHook` (intercettano `Action` virtuale e `ChangeAction`) | `turn_to`, `set_destination`, `physics`, `commands`, `check_move`, `damage`, `area_damage`, `find_target`, `aim_fire`, `create_children` |
| `World::physics(s, blocked, obstacle)` | `physics` (`CheckMove`, `FirstObstacleOnLine`) |
| `World::firstObstacleOnLine(s, x, y, z, probe)` | `first_obstacle` (`AskPlace`) |
| `World::createChildren(s, occupied, create, dest)` | `create_children` |
| `World::commandsDue`, `gridCell`, `setMapSize`, `lastDistance`/`setLastDistance` | `commands`, `physics`, `set_destination`, `aim_fire`, `action_*` |

Le divergenze volute (`docs/engine.md` §5: D10 dirs = 0, `Random(−1)`, direzione stantia del flag 4,
catena dei figli; D13 orologio; D15 puntatori) sono contate e stampate per famiglia ("documented
divergences"), mai ignorate in silenzio. Le famiglie `action_*` confrontano tutti i campi dello
sprite, dell'unità, del bersaglio e della torretta, il valore restituito, lo stato di `rand()` e
`g_dist`.
