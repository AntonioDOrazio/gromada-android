# Il motore di simulazione di `gromada.exe` (issue #2)

Analisi statica di `gromada.exe` (PE32, MSVC, dicembre 1999, image base `0x400000`).
Tutti gli indirizzi sono **indirizzi virtuali** dell'eseguibile originale; gli offset dei campi
sono in esadecimale. Il codice è stato letto con gli script di `tools/re/` (angr + capstone:
Ghidra non era scaricabile dall'ambiente di analisi, vedi "Metodo").

Livelli di confidenza: **alta** = letto direttamente nel disassemblato e coerente con i dati;
**media** = letto nel codice ma con dettagli interpretati (nomi, casi rari);
**bassa** = ipotesi plausibile da indizi indiretti.

---

## 0. Sommario per chi implementa

1. **Non esiste un tick logico globale fisso** (alta). Il main loop (`WinMain` `0x401550`) gira
   alla velocità massima possibile; a ogni iterazione il **clock di gioco** `curclock`
   (`0x42bf94`, millisecondi) avanza del tempo reale trascorso (`timeGetTime`) **limitato a
   71 ms per frame** (`0x401f70`). Se il PC è più lento di ~14 fps il gioco rallenta.
2. **Ogni sprite ha il proprio tick**: viene aggiornato (`SpriteTact` `0x409a20`) quando
   `curclock - sprite.lastTact >= vid.frameDuration` (campo del vid, ms), con jitter se il vid
   ha il flag `Randomized`. Il controllo è fatto **una sola volta per frame** (nessun recupero di
   tick persi). Nei dati 284 vid su 368 hanno `frameDuration = 71 ms` (**≈14 Hz**: è il tick
   "di progetto"); Kassandra (il veicolo del giocatore) e i caccia 39 ms (≈25,6 Hz); missili 28 ms.
   Nelle demo originali registrate i frame durano 29–50 ms, mai più di 71 (alta).
3. A ogni tick dello sprite: avanza **un fotogramma** di animazione; posizione, velocità e quota
   sono integrate con il **dt reale** (`curclock - lastTact`), in interi con accumulatori
   frazionari a 7 bit. Velocità in **pixel per 1024 ms** (≈ px/s). Gravità 174 px/s².
4. **Ordine di un frame** (`Tact` `0x4141f0`): input/demo/rete → script `main()` del livello
   → mondo (`0x40b880`: prima disegna tutti gli sprite, poi esegue i tick di quelli scaduti, per
   strato z 0..15 e in ordine di lista) → UI/scroll → giocatori (comandi al flagman) →
   IA dei gruppi (`0x416730`) → clamp della camera → suoni.
5. **Oggetti**: struct "SPRITE" di `0x3f` byte (packed) + estensioni per classe (fino a `0x64`).
   Una **sola funzione virtuale**: `int Action(int act, int var1, int var2)` (vtable di 1 slot);
   le 21 `ObjectClass` corrispondono a 21 classi C++ create da `CreateUnit` `0x40e1b0`.
   Tabelle dei campi nella sezione 2.
6. **Comandi**: pila di record da 9 byte (`act`, `var1`, `var2`); si esegue l'ultimo; lo
   script `AddCommand` inserisce **in fondo** (ordine FIFO), il motore spinge in cima;
   `act_cyclecommand` in fondo alla pila la rende ciclica (pattuglie).
7. **Niente pathfinding globale** (non trovato): i veicoli vanno dritti al waypoint, al blocco
   fanno retromarcia e sterzano di ±30 passi; la collisione è AABB con quota, su griglia spaziale.
8. **RNG**: il `rand()` della CRT MSVC (`state = state*214013 + 2531011; (state>>16)&0x7fff`,
   `0x41c7b5`), unico per motore e script, seminato a ogni caricamento di mappa con
   `timeGetTime()` (o il seed scritto nella demo, o `0x8f` in rete).

Raccomandazione per il core (60/120 fps): emulare il main loop originale con un **passo
virtuale fisso** (es. 1000/60 ms o un passo da 35 ms simile alle demo) per avanzare `curclock`,
applicare la regola del tick per sprite su quel passo e disegnare interpolando o semplicemente
mostrando lo stato corrente. Il comportamento dipende dal passo scelto (quantizzazione dei
tick e consumo dell'RNG, sezione 10): per la riproduzione esatta delle demo bisogna usare i
`curclock` registrati in ogni frame della demo.

---

## Metodo e strumenti

- `tools/re/gromada_re.py`: CFG e decompilatore di angr, disassemblatore capstone, ricerca di
  riferimenti, decodifica degli `switch` MSVC a due livelli (`switch ADDR`). Comandi
  concatenabili, es. `python3 -I tools/re/gromada_re.py "dis 0x409a20 ; switch 0x40ea3b"`.
- `tools/re/vid_stats.py`: campi di simulazione di tutti i vid e della tabella armi di `fw.res`.
- `tools/re/engine_reference.py`: riferimento eseguibile delle formule intere (direzione CORDIC,
  movimento, clock), con self-test; con i dati verifica che le tabelle `sin/cos` e CORDIC
  calcolate coincidano con quelle dell'exe e applica la formula di moto a vid reali.
- Ghidra headless: il download delle release GitHub e di Maven Central è bloccato dal proxy;
  angr (pip) è stato installato in `/root/tools/re-venv` (fuori dal repo).
- Le demo `maps/demo*.dem` sono state usate come verifica empirica dei tempi (sezione 1.5).

---

## 1. Ciclo principale e tempi

### 1.1 `WinMain` — `0x401550` (alta)

```c
// 0x4016d8
for (;;) {
    while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { if (msg == WM_QUIT) goto quit; ... dispatch }
    if ((appActive || netGame) && !minimized && !videoPlaying) {     // 0x42be74, 0x44e694, 0x42be70, 0x42bf80
        svga.BeginFrame();                    // 0x401f70: avanza curclock, Lock della superficie
        draw = field.Tact(appActive != 0);    // 0x4141f0 (this = 0x439d68)
        svga.EndFrame(draw);                  // 0x406ea0 + 0x402030: effetti, Unlock, Flip
    } else WaitMessage();
}
```

Non c'è nessuna attesa esplicita né limitatore di frame rate: il ritmo lo dà il `Flip` di
DirectDraw (vsync) o la CPU.

### 1.2 Il clock di gioco — `0x401f70` (alta)

```c
uint32_t g_lastReal  /*0x42bf98*/;
uint32_t curclock    /*0x42bf94*/;     // "GetTime()" degli script restituisce questo
void BeginFrame() {
    uint32_t prev = g_lastReal;
    g_lastReal = timeGetTime();
    uint32_t dt = g_lastReal - prev;
    curclock += (dt > 71) ? 71 : dt;    // 0x47
    ...Lock()...
}
```

- `curclock` viene inizializzato dal campo `startTimer` della sezione MapInfo della mappa
  (`0x414dd4`, quindi un salvataggio riprende dal suo tempo) oppure dal valore scritto nella
  demo (`0x414e8b`).
- In rete (`0x4141f0`, ramo `0x4143ee`) ogni giocatore invia `curclock` e input del frame; il
  clock usato da tutti è il **massimo** dei clock ricevuti (lockstep); il valore di `rand()` di
  ogni frame viaggia con il pacchetto per rilevare "Synchro error".
- Lo scroll della camera usa invece il tempo reale (`0x42bf98`), tranne in registrazione o
  riproduzione di demo, dove usa `curclock` (`0x415f87`).

### 1.3 `Tact` del campo di gioco — `0x4141f0` (alta per l'ordine, media per i dettagli UI)

```c
int Field::Tact(int active) {
    mouseWorld = mouseScreen + camera;                       // 0x42bf88.. -> record input 12 byte
    if (demoPlayback) { read u32 time + 12 byte input; aspetta che il tempo reale lo raggiunga;
                        curclock = time; }                    // 0x414252..0x41435a
    else drawThisFrame /*0x4408f4*/ = active;
    if (demoRecording) write curclock + input;               // 0x4143bb
    if (netGame) { scambio clock/input/rand, curclock = max }// 0x4143ee..0x4146ec
    if (!editor && !infoMode) Logic.RunMain();               // 0x41a790: main() dello script
    World.Tact(drawThisFrame);                               // 0x40b880: disegno + tick sprite
    DrawSelection(); DrawGroupsDebug(); DrawWaypoints();     // overlay
    if (!infoMode) Selection.Tact(input);                    // 0x40c1d0
    UI_Tact(input);                                          // 0x415e20: scroll, editor, finestra
    if (!infoMode && !editor && !paused) {
        for (p = 0; p < 4; p++) players[p].Tact(input[p]);   // 0x4169b0: comandi al flagman
        Groups_Tact();                                       // 0x416730: IA delle squadre
    }
    clamp camera ai bordi della mappa;                       // 0x414838
    Sound.Tact();                                            // 0x408f20
    return drawThisFrame;
}
```

### 1.4 Aggiornamento del mondo — `World::Tact` `0x40b880` (alta)

Il mondo è un array di 16 liste concatenate (`0x439cf8[16]`, una per `vid.z_layer`).

```c
uint32_t World::Tact(int draw) {
    if (draw)
        for (layer = 0; layer < 16; layer++)
            for (s = list[layer]; s; s = next) {              // next salvato in 0x439d48
                if (s.state & st_invisible) continue;
                if (!(s.state & 4 /*fisso allo schermo*/) && fuori da ±512 px dal centro vista) {
                    if (s.y - viewCenterY >= 512) break;    // lista ordinata per y
                    continue;
                }
                disegna figli con dz<0, s, figli con dz>=0  // 0x40b620 (sez. 9)
            }
    for (layer = 0; layer < 16; layer++)
        for (s = list[layer]; s; s = next) {
            Vid& v = vids[s.nvid];
            if (v.flags & SkipUpdate) continue;
            uint32_t period = v.frameDuration;               // campo +0x108 del vid in memoria
            if (v.flags & Randomized) period += (period >> 3) - rand() % ((period >> 2) + 1);
            if (curclock - s.lastTact >= period) s.Tact();   // 0x409a20
        }
}
```

Note importanti:
- **Disegno prima dell'aggiornamento**: lo stato mostrato in un frame è quello del frame
  precedente.
- Il tick è **al più uno per frame per sprite**; il `dt` usato dentro `Tact` è comunque il
  tempo vero trascorso dall'ultimo tick (sez. 4), quindi spazi e velocità sono indipendenti dal
  frame rate, mentre animazioni e decisioni (IA, comandi, spari) scattano ai bordi dei frame.
- `rand()` del jitter viene chiamato **a ogni frame** per ogni sprite `Randomized`, anche se
  non scatta il tick (importante per il determinismo).
- `frameDuration` viene letto da `fw.res` e diviso per `1000/[0x42838c]` con `[0x42838c] = 1000`
  (`0x406683`): resta in millisecondi.

### 1.5 Verifica empirica sulle demo (alta)

Le demo registrano per ogni frame `u32 curclock` + 12 byte di input (sezione 7 di `.dem`,
dettagli nella sezione "Per la issue #3"). Statistiche dei Δ fra frame consecutivi:

| demo | frame | durata | Δ medio | Δ più frequenti | Δ max |
|---|---|---|---|---|---|
| demo1 | 5950 | 234,8 s | 39,5 ms | 29, 32, 28, 30, 33 | **71** (45 volte) |
| demo2 | 6390 | 234,4 s | 36,7 ms | 31, 32, 30, 33, 34 | **71** (73 volte) |
| demo3 | 5301 | 177,1 s | 33,4 ms | 31, 32, 30, 29, 33 | **71** (15 volte) |

Il Δ non supera mai 71: conferma il limite di `0x401f70`. Le demo sono state registrate a
~25–35 fps.

### 1.6 Pausa — `Pause()` `0x40ac70` (alta)

`Pause(1)` salva `pauseStart = curclock` e alza `g_paused` (`0x439d38`). Durante la pausa
`curclock` continua ad avanzare ma `SpriteTact` salta fisica, comandi e figli per tutti gli sprite
tranne quelli "fissi allo schermo" (`state & 4`: menu, cursori). `Pause(0)` fa
`s.lastTact += pauseStart - curclock` per ogni sprite (e catena di figli) e
`curclock = pauseStart`. Poiché `World::Tact` dà il tick a tutti gli sprite anche in pausa
(`lastTact = curclock` a ogni tick), lo spostamento conserva il tempo dall'ultimo tick: nessun
salto (correzione della issue #13, verificata sulle demo che vanno in pausa).

---

## 2. Oggetti

### 2.1 `Vid` in memoria (alta)

Tabella `vids` all'indirizzo in `0x42df57`, `0x136` (310) byte per vid, numero in `0x42df4f`.
Il caricatore `0x406110` legge campo per campo **lo stesso layout del file** (quello di
`src/libgromada/resources.hpp`) fino a `0x106`, poi riusa i byte da `0x107`:

| off | campo | note |
|---|---|---|
| 0x00 | name[34] | CP866 |
| 0x22 | category | `ObjectCategory` (bit) |
| 0x23 | class | `ObjectClass`, sceglie la classe C++ |
| 0x24 | flags (u16) | `ObjectFlags`, semantica in 2.6 |
| 0x26 | collisionMask | vedi sez. 6 |
| 0x27/29/2b | sizeX/Y/Z (s16) | |
| 0x2d | maxHP (u8) | 0 = invulnerabile |
| 0x2e | visibilityRadius (u16) | raggio di ricerca bersagli |
| 0x31 | speedX (s16) | velocità massima orizzontale, px/1024 ms; 9999 = istantanea |
| 0x33 | speedY (s16) | velocità verticale massima |
| 0x35 | acceleration (u16) | px/1024 ms²; 9999 = istantanea |
| 0x37 | rotationPeriod (s8) | **dopo il caricamento** vale `rotationPeriod*6/directionsCount`: passo in px dopo il quale un'unità in moto rivaluta la rotta (sez. 4.6) |
| 0x38 | army | esercito di default |
| 0x39 | weapon | indice nella tabella armi |
| 0x3b | deathDamageRadius (u16) | |
| 0x3d | deathDamage (u8) | |
| 0x3e..0x40 | linkX/Y/Z (s8) | offset del sotto-oggetto collegato (torretta) |
| 0x41 | linkedObjectVid (u16) | |
| 0x45 | directionsCount | |
| 0x46 | z_layer | 0..15 (≥16 = errore "SPRITE::z>MaxZ") |
| 0x47 | animationLengths[16] | |
| 0x57 | nsfx[16] | suono all'inizio di ogni ciclo dell'azione |
| 0x77/0x97/0xb7 | childrenOffsets X/Y/Z [16] | per azione |
| 0xd7 | childNvid[16] | per azione; <0 = offset casuali |
| 0xf7 | childrenCount[16] | |
| 0x107 | dataFormat (u8) | sovrascrive il byte basso di `dataSizeOrNvid` |
| 0x108 | **frameDuration** (u16, ms) | **periodo del tick** |
| 0x10a | numOfFrames | |
| 0x10c/0x10e | width/height | |
| 0x110 | dataSize | |
| 0x114 | puntatori ai fotogrammi | |
| 0x118 | dati (palette hicolor 512 byte + fotogrammi) | caricati al primo uso (`0x406830`) |
| 0x11c | offset nel file; 0x120 = caricato | |
| 0x121/0x123 | sizeX/2, sizeY/2 | calcolati (`0x4066c5`) |
| 0x125 | abs(sizeZ) | |
| 0x127 | 256/directionsCount | ampiezza di un passo di direzione |
| 0x128 | maxUnit (int, -1 = illimitato) | `MaxUnit()` |
| 0x12c | numero di istanze vive | |
| 0x130 | nvid sorgente della grafica (o -1) | se `dataSizeOrNvid < 0` copia `0x107..0x110` dal vid sorgente |
| 0x132 | palette alternative (4×512 byte) | `st_gamma`, `SetVid(VID_GAMMA..)` |

### 2.2 Struttura `SPRITE` (base, `0x3f` byte, packed) (alta salvo dove indicato)

| off | tipo | campo | uso |
|---|---|---|---|
| 0x00 | ptr | vtable | 1 solo slot: `Action` |
| 0x04 | ptr | next | lista dello strato z |
| 0x08 | ptr | child | catena dei sotto-oggetti (torrette, fumo, ...) |
| 0x0c/0x0e/0x10 | s16 | x, y, z | mondo, pixel |
| 0x12 | u8 | direction | 0 = nord, 64 = est, senso orario |
| 0x13 | u8 | action | azione d'animazione corrente (0..15) |
| 0x14 | u16 | nvid | |
| 0x16 | s16 | frame | fotogramma corrente |
| 0x18 | s16 | firstFrame | |
| 0x1a | s16 | lastFrame | |
| 0x1c | u16 | state | bit in 2.5 |
| 0x1e | u32 | lastTact | `curclock` dell'ultimo tick |
| 0x22 | s16 | speed | velocità attuale (<0 = retromarcia) |
| 0x24 | s16 | vz | velocità verticale |
| 0x26/0x27/0x28 | s8 | fracX/fracY/fracZ | accumulatori frazionari (1/128 px) |
| 0x29 | u8 | stepAccum | px percorsi dall'ultima rivalutazione |
| 0x2a/0x2b/0x2c | s8 | dx/dy/dz | offset rispetto al padre (se è un figlio) |
| 0x2d/0x2f/0x31 | s16 | goX/goY/goZ | destinazione del moto |
| 0x33 | u16 | timer | conto alla rovescia in ms (ricarica armi, costruzione, carburante) |
| 0x35/0x37/0x39 | s16 | attackX/Y/Z | punto di mira (per `act_coor_attack`, figli di `act_fight`) |
| 0x3b | ptr | commands | pila dei comandi (sez. 3.3) |
| 0x3f | u8 | hp | classi "materiali" (size ≥ 0x40) |

Estensione delle unità (Vehicle, Building, AviaVehicle, Mine, Kassandra; size `0x54+`) (alta per
i campi usati, media per i nomi):

| off | tipo | campo |
|---|---|---|
| 0x40 | ptr | target (`act_gettarget`) / per Building: unità in carico |
| 0x44 | u8 | army (`act_setarmy/getarmy`, init `vid.army`) |
| 0x45 | u8 | behave (init 1; aviazione 5), bit in 8.1 |
| 0x46/0x48 | s16 | waypointX/Y (-1 = nessuno) |
| 0x4a | u8 | buildTime in secondi (init 20) |
| 0x4b | s8 | sterzata residua dopo un blocco (±30) |
| 0x4c | {s16 n, s16 cap, s16* v} | items (`act_additem/deleteitem/askitem`) |
| 0x54 | u16 | Building: nvid in costruzione (size `0x56`) |
| 0x54..0x63 | | Kassandra (size `0x64`): armi/munizioni, non analizzati in dettaglio (bassa) |

Le classi Superstructure (9), Image (10), Effect2 (12) usano `0x3f` byte (niente hp), Font
`0x45`, AviaMissile `0x44`, le altre "materiali" `0x40`.

### 2.3 Classi e funzioni `Action` — `CreateUnit` `0x40e1b0` (alta)

| class | nome | size | vtable | Action | azioni gestite in proprio (altrimenti → classe base) |
|---|---|---|---|---|---|
| 0 | Terrain | 0x40 | 0x426208 | 0x412580 | (base materiale) |
| 1 | Static | 0x40 | 0x426214 | 0x412580 | (base materiale) |
| 2 | Vehicle | 0x54 | 0x42620c | 0x40ea20 | 15 31 32 33 34 36 38 39 40 43 45 46 48 51 52 54 56 60 67 70 74 75–78 81 |
| 3 | Building | 0x56 | 0x426210 | 0x410e10 | 41 43 45 47 49 51 54 83 84, poi Vehicle |
| 4 | AviaVehicle | 0x54 | 0x426204 | 0x4105a0 | 31 38 39 40 43 45 46 48 51 52 54 57 60 75–78, poi Vehicle |
| 5 | Projectile | 0x40 | 0x426200 | 0x40f710 | 31 38 39 40 45 46 57 |
| 6 | CannonMissile | 0x40 | 0x4261fc | 0x40ff60 | 2 31 38 39 40 45 46 48 |
| 7 | DownedAvia | 0x40 | 0x4261f8 | 0x40fd70 | 2 31 38 45 46 |
| 8 | NA | 0x40 | 0x4261f4 | 0x410d00 | 2 31 38 45 46 |
| 9 | Superstructure | 0x3f | 0x4261c4 | 0x40d9e0 | (base sprite) |
| 10 | Image | 0x3f | 0x4261f0 | 0x40dd30 | 0–5 45 46 51 54 58 |
| 11 | Effect1 | 0x40 | 0x4261ec | 0x40fec0 | 45 46 57 |
| 12 | Effect2 | 0x3f | 0x4261e8 | 0x412a30 | 57 |
| 13 | Mine | 0x54 | 0x4261e4 | 0x411560 | 31 38 45 57, poi Vehicle |
| 14 | Shell | 0x40 | 0x4261e0 | 0x40fc70 | 31 38 45 46 57 |
| 15 | AviaMissile | 0x44 | 0x4261dc | 0x4101e0 | 2 31 38 45 46 48 57 |
| 16 | Debris | 0x40 | 0x4261d8 | 0x40fb50 | 2 31 38 45 46 |
| 17 | Kassandra | 0x64 | 0x4261d4 | 0x411a60 | 15 31 32 35 40 43 45 46 54 56 63 64 70, poi Vehicle |
| 18 | Bonus | 0x40 | 0x4261d0 | 0x4117e0 | 0 11 31 45 |
| 19 | Font | 0x45 | 0x4261cc | 0x40dea0 | 32 45 46 54 71 72 73 |
| 20 | RepairCannon | 0x40 | 0x4261c8 | 0x40f9c0 | 15 57, poi Projectile |

Catena delle basi: classe → `0x412580` "materiale" (hp, danni, morte, salvataggio) →
`0x40d9e0` "sprite" (azioni comuni). Le liste dei casi sono state estratte con
`gromada_re.py switch` sulle tabelle di salto.

```c
SPRITE* CreateUnit(int nvid, int x, int y, int z, int action, int dir, SPRITE* parent) { // 0x40e1b0
    Vid& v = vids[nvid];
    if (v.maxUnit >= 0 && v.count >= v.maxUnit) return 0;          // MaxUnit()
    if (v.flags & Randomized) { x += 8 - rand()%17; y += 8 - rand()%17; }
    if (x < 0 || y < 0 || x > mapW || y > mapH) return 0;           // 0x428e0c/0x428e10
    SPRITE* s = new <classe di v.class>;                            // tabella 0x40e65c
    v.count++;
    s->Init(nvid, v.animationLengths[act_birth] ? act_birth : action, dir, x, y, z); // 0x40bf30
    if (parent) s->state |= parent->state & st_gamma;
    if (v.childNvid[act_birth] && !paused) { a = s->action; s->action = act_birth;
                                             CreateChildren(s); s->action = a; }   // 0x40a950
    if (v.z_layer == 5) PushFront(list[5], s) else InsertSorted(s);  // 0x40bcf0 / 0x40bd40
    if (v.flags & PresentOnGrid) Grid.Add(s);                        // 0x40d870
    return s;
}

void SPRITE::Init(nvid, action, dir, x, y, z) {                     // 0x40bf30
    this->nvid = nvid; x,y,z = goX,goY,goZ = ...; lastTact = curclock;
    this->action = action; this->direction = dir; tutto il resto = 0; commands = 0;
    dx,dy,dz = vid.linkX,linkY,linkZ;
    if (vid.linkedObjectVid) {                       // torretta o sovrastruttura
        child = new Superstructure; child->Init(vid.linkedObjectVid, 0, dir, x+dx, y+dy, z+dz);
        if (vid.flags & 0x400) InsertSorted(child);  // "InvisibleSubobjects", vedi 2.6
        child->state |= 8;                           // sotto-oggetto
    }
    SetAnimation();                                  // 0x40b330
    if (action == act_stand && lastFrame > firstFrame && !(class Image && 2 frame))
        frame += rand() % (lastFrame - firstFrame + 1);   // fase casuale dell'animazione
    Action(act_init, 0, 0);
}
```

L'"identificatore unico" restituito agli script da `Create`, `GetObject`, `Flagman`, ... è il
**puntatore** allo sprite (`0x412cc0`). Nelle mappe gli id della sezione ObjectsIds vengono
tradotti in puntatori al caricamento.

### 2.4 Il tick dello sprite — `SPRITE::Tact` `0x409a20` (alta)

È il cuore della simulazione. Pseudocodice completo (le parti di fisica sono dettagliate nella
sezione 4):

```c
int SPRITE::Tact() {
    if (child && child->Tact()) {               // il figlio è morto: ri-aggancia i nipoti
        child = g_orphan; dx += g_odx; dy += g_ody; dz += g_odz;
    }
    if (!(state & 0x200) && vid.nsfx[action]) { // suono una volta per ciclo d'animazione
        state |= 0x200; PlayPositional(nsfx, pan = 4*dxSchermo, att = -2*max(|dx|,|dy|));
    }
    frame++;
    dt = curclock - lastTact;
    if (timer) timer = (dt > timer) ? 0 : timer - dt;
    if (!paused && vid.childNvid[action] && (frame > lastFrame || (vid.flags & SpawnChildren)))
        CreateChildren();                        // 0x40a950, sez. 7.4
    if (frame > lastFrame && action == act_death) {
        if (vid.class == Effect2) { salva la catena dei figli in g_orphan...; child = 0; }
        else if (state & 8) goto skip;           // i sotto-oggetti muoiono col padre
        else g_orphan = 0;
        Action(act_death, 0, 0);                 // effetti di morte (danno ad area, bonus)
        Action(act_destroy, 0, 0);
        return 1;
    }
  skip:
    if ((!paused || (state & 4)) &&
        !(vid.category <= Object && !(state & 8)) && !(state & st_passive)) {
        Physics();                               // gravità, velocità, quota, moto, griglia (sez. 4)
        if (frame > lastFrame || vid.class < 6)  // Terrain..Projectile: ogni tick
            NextCommand();                       // pila comandi o act_nextcommand (sez. 3.3)
        if (vid.flags & Calltact) Action(act_calltact, 0, 0);
    } else if (vid.class == Shell && frame > lastFrame) ChangeAction(act_stand, -1);
    lastTact = curclock;
    if (frame > lastFrame) {
        frame = firstFrame;
        if (!paused && vid.class != Image) state &= ~0x200;
    }
    for (c = child, p = this; c; p = c, c = c->child)   // i figli seguono il padre
        c->x = p->x + p->dx, c->y = p->y + p->dy, c->z = p->z + p->dz;
    return 0;
}
```

Quindi un'animazione da N fotogrammi dura N tick (N × `frameDuration` ms, quantizzati ai frame).
Le categorie Terrain (1) e Object (2) non hanno fisica né comandi, salvo i sotto-oggetti.

### 2.5 Bit di `state` (`+0x1c`) (alta salvo dove indicato)

| bit | nome (header.ini se esiste) | significato |
|---|---|---|
| 0x0001 / 0x0002 | — | arrivato in x / in y (la destinazione è stata attraversata) |
| 0x0004 | — | coordinate di schermo (menu, cursori, UI): niente culling, aggiornato anche in pausa |
| 0x0008 | — | sotto-oggetto (figlio) |
| 0x0010 | `st_move` | in movimento verso `goX/goY` |
| 0x0020 | — | sta attaccando (`target` o punto `attackX/Y`) |
| 0x0040 | `st_passive` | niente fisica/comandi (es. unità dentro una fabbrica) |
| 0x0080 | — | Building: costruzione in corso |
| 0x00f0 | `st_command` | maschera "sta eseguendo un ordine" |
| 0x0100 | — | accelera (altrimenti decelera) |
| 0x0200 | — | suono del ciclo già emesso |
| 0x0400 | `st_invisible` | non disegnato, ignorato da FindTarget |
| 0x0800 | — | Vehicle/Kassandra: in salto (`act_jump`) |
| 0x3000 | `st_gamma` | indice della palette alternativa (0..3) |

### 2.6 `ObjectFlags` (campo `vid.flags`) secondo il codice (alta salvo nota)

| flag | uso osservato |
|---|---|
| 0x0001 RandomDirection | ruota istantaneamente (`TurnTo` `0x40b0b0`, `SetDestination`); i figli nascono con direzione casuale (`0x40a950`) |
| 0x0002 Gravity | `vz -= 174·dt/1024` quando `z != 0`; traiettorie balistiche (sez. 4.3) |
| 0x0004 Calltact | `Action(act_calltact)` a ogni tick |
| 0x0008 SkipUpdate | lo sprite non riceve mai tick |
| 0x0010 SpawnChildren | i figli dell'azione corrente nascono a ogni tick invece che a fine ciclo |
| 0x0020 NoCollision | non collide né blocca |
| 0x0040 PresentOnGrid | inserito nella griglia spaziale; `act_destroy` avvisa tutti con `act_deletetarget` |
| 0x0080 Shadow | disegna l'ombra (sez. 9.3) |
| 0x0100 Randomized | ±8 px alla creazione e jitter del periodo di tick |
| 0x0200 ZeroZ | disegnato ignorando z |
| 0x0400 InvisibleSubobjects | i figli non sono disegnati dal padre; il sotto-oggetto collegato viene invece inserito nelle liste del mondo come sprite indipendente (media) |
| 0x0800 IgnoreGlobalGamma | la palette non riceve la gamma globale della mappa (`0x406903`) |
| 0x1000 Wind | spinto dal vento (`SetWind`) |
| 0x2000 SkipMapEditor | non usato nel motore |
| 0x4000 CrushOnCollision | quando un'unità in moto lo tocca riceve `act_damage(2)` invece di bloccarla (`0x40a5c0`) |

### 2.7 Distruzione (alta)

`act_destroy` (base `0x40dac1`): `vid.count--`, rimozione dalla griglia, liberazione dei comandi,
rimozione dalla lista z (`0x40be60`), `act_destroy` al figlio, `free`. Le unità (`0x40ec2b`)
prima svuotano gli items, escono dai gruppi (`0x4166a0`), dai flagman dei giocatori e dalla
lista `0x43a6cc`. Building distrugge anche l'unità in carico. Avviene solo a fine animazione di
`act_death` (o subito se chiamato direttamente da script/codice).

---

## 3. Azioni

### 3.1 Smistamento (alta)

Ogni classe ha una sola funzione virtuale `int Action(int act, int var1, int var2)`
(`call [vtable+0]`); il valore di ritorno serve alle azioni `get*`. Ogni funzione è uno `switch`
MSVC che gestisce alcune azioni e passa le altre alla classe base (tabella 2.3).
Le azioni 0..15 non sono comandi ma **animazioni**: `ChangeAction(act, dir)` `0x40b3e0`.
La builtin di script `Action(unit, act, v1, v2)` (`0x412c51`) chiama `unit->Action(act,v1,v2)` se
`act > 15`, altrimenti `ChangeAction(act, -1)`.

### 3.2 `ChangeAction` `0x40b3e0` e `SetAnimation` `0x40b330` (alta)

```c
void ChangeAction(int act, int dir /* -1 = attuale */) {
    state &= ~0x200;
    if (child) { if (act == death) child->ChangeAction(death, -1);
                 if ((act == open || act == close || act == stand_open)
                     && vids[child->nvid].animationLengths[act]) child->ChangeAction(act, -1); }
    if (dir == -1) dir = direction;
    if (act == action && dir == direction) return;
    if (action == stand_open && !vid.animationLengths[act]) { direction = dir; action = act; }
    else { direction = dir; action = act; SetAnimation(); }
    // morte/ferita/nascita senza animazione: frame = lastFrame (finiscono al tick dopo)
}
void SetAnimation() {             // identico a getAnimationFrameRange di visual_logic.hpp
    a = vid.animationLengths[action] ? action : 0;
    first = sum(animationLengths[0..a-1]) * dirs
          + (((direction + (256/dirs)/2) & 0xff) * dirs >> 8) * animationLengths[a];
    frame = firstFrame = first; lastFrame = first + animationLengths[a] - 1;
}
```

### 3.3 Pila dei comandi (alta)

Memoria (`sprite+0x3b`): `{ s16 count; s16 capacity; struct { u8 act; i32 var1; i32 var2; } e[]; }`
(record da 9 byte da `+4`). L'elemento **in cima** è `e[count-1]`.

Esecuzione (in `SpriteTact`, `0x40a12a`), solo se `frame > lastFrame || class < 6`:

```c
if (commands && (!(state & st_command) ||
                 (!(state & st_move) && commands->top().act == act_submove))) {
    count--;
    if (e[count].act == act_cyclecommand) count = e[count].var1;   // ritorna alla posizione var1
    if (e[count].act > 15) Action(e[count].act, e[count].var1, e[count].var2);
    else                   ChangeAction(e[count].act, -1);
    if (count == 0) free(commands), commands = 0;
} else if ((state & 3) == 3 || (class != Vehicle && class != Avia && class != Projectile)
           || stepAccumDiQuestoTick >= vid.rotationPeriod /*passo*/) {
    Action(act_nextcommand, 0, 0);
}
```

- Un comando viene prelevato solo quando lo sprite è libero (`state & 0xf0 == 0`), o quando è
  fermo e il prossimo è `act_submove` (che quindi si concatena senza attendere).
- `act_cyclecommand(n)`: messo in fondo alla pila (`e[0]`), quando viene raggiunto riporta
  `count` a `n` → la sequenza `e[n]..e[1]` si ripete all'infinito (pattuglie). Non viene eseguito
  come azione.
- Inserimenti:
  - `AddCommand` dello **script** (`0x413008` → `0x40c7c0`): **in fondo** (`e[0]`), spostando gli
    altri; ignorato se `e[0]` è già `act_cyclecommand`. Quindi gli ordini dati in sequenza
    dallo script vengono eseguiti nello stesso ordine.
  - Codice del motore (`0x40c890`): **in cima**, eseguito per primo (es. Building:
    `AddCommand(stand_open); AddCommand(unload); ChangeAction(open)` → apre, scarica, resta aperto).
  - Waypoint del giocatore/editor (`0x40c690`): come lo script, ma se il nuovo punto (`act_move`
    o `act_coor_attack`) cade a meno di 24×16 px dalla posizione dell'unità o da un waypoint già
    in lista, aggiunge `act_cyclecommand` chiudendo il giro.
  - Mappe: la sezione Command viene letta **così com'è** (`0x4151d2`): il primo comando eseguito
    è l'ultimo del file.
- `act_clearcom` (66) libera la pila; `act_deletetarget(t)` (Vehicle/Avia) toglie i comandi
  `act_attack(t)` e azzera il target.

### 3.4 Azioni comuni (base `0x40d9e0` e materiale `0x412580`) (alta)

| act | effetto |
|---|---|
| 31 path_block | base materiale: `ChangeAction(stand)` |
| 37 changeaction(a,d) | `ChangeAction(a, d)` |
| 38 path_limit(dx,dy) | base materiale: si sposta di (dx,dy) e muore (gli oggetti usciti dalla mappa esplodono) |
| 43 damage(n) | se `maxHP==0` nulla; se `n >= hp` → `ChangeAction(death)`; altrimenti `hp -= n` e, se `hp` scende a ≤ maxHP/2 → `ChangeAction(wound)` |
| 45 nextcommand | se non sta morendo: in moto o `speed != 0` → `act_go`, altrimenti `act_stand` (salvo `stand_open`) |
| 46 init | `hp = maxHP` |
| 15 death (a fine animazione) | danno ad area, sez. 7.5 |
| 54 destroy | sez. 2.7 |
| 62 changevid(n, a) | cambia nvid; `a>=0` → `ChangeAction(a)` altrimenti ricalcola l'animazione |
| 65 gethp / 76 getarmy / 78 getbuildtime | hp / `vid.army` / 1 |
| 68 changecoor(x,y) | teletrasporto, aggiorna la griglia |
| 69 getlink | puntatore al figlio |
| 79/80 setstate/getstate, 82 getaction | |
| 51/52/58/59 | salvataggio (issue #3) |

---

## 4. Movimento e fisica

### 4.1 Unità e tabelle (alta)

- Direzione 0..255: **0 = nord (y decrescente), 64 = est, 128 = sud, 192 = ovest**.
- `SIN[i] = (int)(1024·sin(2πi/256))` a `0x428e14`, `NEG_COS[i] = -(int)(1024·cos(2πi/256))` a
  `0x429214` (troncati: verificato, 0 differenze). `0x429614`/`0x429a14` sono le stesse
  moltiplicate per `scaleY/scaleX` (= 16/24) a ogni caricamento di mappa (`0x414f38`), usate solo
  per ruotare gli offset dei figli (prospettiva).
- `GetDirection(x1,y1,x2,y2)` `0x40b030`: CORDIC a 7 iterazioni (angoli `{64,37,19,10,5,2,1}` in
  1/512 di giro, `0x429e18`), restituisce `angolo>>1` e mette la **distanza euclidea**
  `(x·636750)>>20` in `g_dist` (`0x439d4c`). Implementazione esatta in
  `tools/re/engine_reference.py`.
- Distanza "di gioco" (portata, visibilità, squadre) `0x40aff0`/`0x40afc0`:
  `max(|dx|, 3·|dy|/2)` (la y vale 1,5 volte: prospettiva).
- Velocità: `speed` in **px per 1024 ms**. Nel moto `dx = (speed·dt·SIN[dir]) >> 13` in unità
  di 1/128 px, cioè `speed·dt/1024` px. Esempi reali (`engine_reference.py`): Gun tank
  speedX 80 (≈78 px/s), Fighter 170, Kassandra BULL 100.

### 4.2 Fisica di un tick (`0x409cbb..0x40a116`) (alta)

```c
dt = curclock - lastTact;
if ((vid.flags & Gravity) && z) vz -= (dt*174) >> 10;                       // gravità
if (state & 0x100) {                                                         // accelera
    if (speed < vid.speedX) speed = vid.acceleration == 9999 ? vid.speedX
                                  : speed + ((vid.acceleration*dt) >> 10);  // può superare speedX
} else if (speed > 0) {                                                      // decelera (x2)
    speed = vid.acceleration == 9999 ? 0 : max(0, speed - ((vid.acceleration*dt) >> 9));
}
if (vz) {                                                                    // quota
    a = ((vz*dt) >> 3) + fracZ; fracZ = a & 127; dz = a >> 7;
    if (z + dz < 0 && z >= 0) Action(act_path_block, 0, 0);                  // tocca terra
    else if (vid.flags & Gravity) z += dz;
    else { /* si ferma esattamente a goZ */ if (oltrepassa goZ) { z = goZ; vz = 0; } else z += dz; }
}
if (speed == 0 && !(windStrength && (vid.flags & Wind))) step = 0x7fff;     // fermo
else if (speed != 9999) {
    d = speed*dt;
    ax = ((d*SIN[dir]) >> 13) + fracX;  ay = ((d*NEG_COS[dir]) >> 13) + fracY;
    if (vid.flags & Wind) { w = windStrength /*0x439d54*/ * dt;
        ax += (w*SIN[windDir /*0x429e14*/]) >> 13; ay += (w*NEG_COS[windDir]) >> 13; }
    fracX = ax & 127; fracY = ay & 127; mx = ax >> 7; my = ay >> 7;
    step = (|speed*dt| >> 10) + stepAccum;                                   // px percorsi
    stepAccum = step >= vid.rotationPeriod ? step - vid.rotationPeriod : step;
    if (state & st_move) {                                                   // attraversa goX/goY?
        if (goX fra x e x+mx) state |= 1;  if (goY fra y e y+my) state |= 2;
    }
    if (x+mx, y+my dentro [0..mapW]x[0..mapH]) {
        if (blocker = CheckMove(x+mx, y+my, z)) Action(act_path_block, mx, my);   // 0x40a5c0
        else { x += mx; y += my; }
    } else Action(act_path_limit, mx, my);
} else {                                                                     // speed 9999: raggio
    p = FirstObstacleOnLine(goX, goY, goZ);                                  // 0x40a770, passi da 16 px
    (x,y) = p ? p : (goX,goY); state |= 3; z = goZ;
}
if (vid.flags & PresentOnGrid && cella cambiata) Grid.Move(this);
```

Il vento (`SetWind(forza, direzione)` `0x413561`) è una velocità costante aggiunta agli sprite con
flag `Wind`, che si muovono anche con `speed = 0`.

### 4.3 Destinazione — `SetDestination(x,y,z)` `0x409770` (alta)

```c
state &= ~3;
if (vid.speedX) {
    if (dirs != 1 && !(flags & RandomDirection)) GetDirection(...);         // solo distanza
    else direction = GetDirection(...);                                      // gira subito
    if (g_dist == 0) return;
    state |= st_move | 0x100;
}
goX, goY, goZ = x, y, z;  fracX = fracY = fracZ = 0;
if ((category == Cannon || class == AviaMissile) && g_dist && speedX) {      // tiro balistico
    if (flags & Gravity) vz = speedX*(z - this.z)/g_dist + (g_dist*174/speedX)/2;
    else                 vz = speedX*(z - this.z)/g_dist;
    vz = clamp(vz, -vid.speedY, vid.speedY);
} else vz = (z > this.z) ? speedY : (z < this.z) ? -speedY : 0;
if (posizione != destinazione) ChangeAction(speed ? act_go : act_start_move, -1);
```

`Stop` `0x409950`: azzera `st_move`, `0x100` e i bit di arrivo, `go = posizione`, `vz = 0`.

### 4.4 Salto — `act_jump(x,y)` Vehicle `0x40f36a`, Kassandra `0x411ad3` (alta)

Solo se `z == 0` e `speedX != 0`: imposta la destinazione come `act_move`, `state |= 0x800`,
`direction = GetDirection`, `ChangeAction(act_fight, dir)` (usa l'animazione di fuoco),
`speed = speedX·1,5`, `vz = (g_dist·174/speed)/2` → parabola che ricade a destinazione per la
gravità. All'atterraggio `act_path_block` con `vz != 0` e bit `0x800`: azzera `speed`, `vz` e il bit.

### 4.5 Coordinata z (alta)

`z` è la quota sopra il terreno (non c'è altimetria del terreno). Lo sprite è disegnato `z`
pixel più in alto (salvo `ZeroZ`). L'aviazione vola a `goZ = 80`; quota 0 = a terra.

### 4.6 Rotazione — `TurnTo(dir)` `0x40b0b0` (media)

Se `dirs == 1` o `RandomDirection`: `direction = dir` subito. Altrimenti confronta gli indici di
direzione quantizzati: se coincidono imposta la direzione esatta e restituisce 1; se no avvia
`act_l_rotate`/`act_r_rotate` verso il lato più breve con la direzione del passo successivo
(tabelle `0x431bf4`/`0x435bf4`, 256 byte per ogni `dirs`) e restituisce 0. La rotazione procede
quindi di un passo di direzione per ogni ciclo dell'animazione di rotazione. In moto, la rotta è
rivalutata (`act_nextcommand`) ogni `vid.rotationPeriod·6/dirs` px percorsi, il che limita il
raggio di sterzata.

---

## 5. Percorsi

**Pathfinding globale: non trovato.** Nessuna funzione esplora la griglia (A*, flood fill...);
le uniche strutture sono la griglia spaziale per le query e i waypoint. Piste seguite: tutte le
chiamate a `0x40a770` (linea libera), `0x40a430` (posto libero), `0x40e9a0`, i riferimenti a
`0x431ab0` (griglia). (alta)

### 5.1 `act_move` e `act_go` (alta)

- `act_go` (2) è solo un'animazione.
- `act_move(x,y)` (Vehicle `0x40f2e5`): toglie il target, `Approach(x,y)` `0x40e9a0`: se la
  destinazione (allargata di metà ingombro) è dentro la mappa, `waypoint = (x,y)` e
  `SetDestination(x, y, z)`. Aviazione (`0x410695`): `SetDestination(x, y, 80)`.
- `act_submove(x,y)`: come move ma conserva il target (si concatena ai comandi, sez. 3.3).
- A ogni `act_nextcommand` (Vehicle `0x40ed35`): se il waypoint è entro un tile
  (`|dx| <= scaleX`, `|dy| <= scaleY`, cioè 24×16 px, `0x428e04/08`) → `ClearWaypoint` (`Stop`,
  `act_stand`); se non è in moto ma ha un waypoint → `SetDestination(waypoint)`; se in moto,
  ruota verso `go` (o di una sterzata residua, vedi sotto) e mette `act_go`.

### 5.2 `act_path_block(dx,dy)` — aggiramento locale (Vehicle `0x40f424`) (alta)

```c
if (vz) { if (state & 0x800) { state &= ~0x800; speed = 0; } vz = 0; return; }   // atterraggio
speed >>= 1; direction += 128;                       // si gira di 180° a velocità dimezzata
if (turn == 0) {
    for (i = 0; i < 4; i++) {                        // prova a sinistra di 32,64,96,128 px
        p += 32 px verso direction-64;
        if (!FirstObstacleOnLine(p -> waypoint)) { turn = -30; break; }
    }
    if (i == 4) turn = +30;
}
```

Nelle rivalutazioni successive `turn` viene consumato di 1 alla volta e sommato alla direzione
desiderata (sterzata progressiva di 30 passi). Un ramo separato gestisce la vera retromarcia
(`speed < 0`, usata dal giocatore): se la direzione verso il waypoint è entro ±16 e 32 px
davanti sono liberi, riparte verso il waypoint.
`act_path_limit` sulle unità di terra: `ClearWaypoint` (si fermano al bordo).
Aviazione: `act_path_block` viene ignorato (passa sopra), `act_path_limit` la fa morire solo a
120×100 px fuori dalla mappa.

### 5.3 Griglia spaziale `0x431ab0` (alta)

Costruita a ogni mappa (`0x40d580`): la cella è la più piccola potenza di 2 ≥ metà del massimo
`sizeX`/`sizeY` fra i vid `PresentOnGrid` ("Grid:: cellx=%i,celly=%i,nox=%i,noy=%i"). Ogni cella è
un vettore di puntatori (8 byte: `{s16 n, s16 cap, ptr}`); `0x40d6d0`/`0x40d7a0` iterano gli
sprite delle celle che coprono un rettangolo (allargato di una cella). Lo stesso oggetto contiene
anche la lista di tutte le unità `PresentOnGrid` delle categorie Vehicle/Avia (`category & 12`),
usata da FindTarget e da `act_destroy`.

---

## 6. Collisioni — `CheckMove(x,y,z)` `0x40a5c0` (alta)

```c
SPRITE* CheckMove(int x, int y, int z) {           // AskPlace 0x40a430 è uguale ma senza "crush"
    if (!(vid.category & (Vehicle|Avia|Cannon))) return 0;   // solo queste categorie collidono
    if (z < 0) return this;
    if (vid.flags & NoCollision) return 0;
    for (s in Grid.Query(x - halfX, y - halfY, x + halfX, y + halfY)) {
        Vid& o = vids[s.nvid];
        if ((o.flags & NoCollision) || s.action == act_death) continue;
        if (|x - s.x| >= o.halfX + halfX || |y - s.y| >= o.halfY + halfY) continue;
        if ((o.category & Terrain) && z == 0) {            // tile di terreno
            if (!(o.collisionMask & vid.collisionMask)) return s;   // terreno non percorribile
        } else if (s != this &&
                   |2*(z - s.z) - o.sizeZ + vid.sizeZ| < o.absSizeZ + vid.absSizeZ) {
            if (o.flags & CrushOnCollision) s.Action(act_damage, 2, 0);   // schiacciato
            else return s;
        }
    }
    return 0;
}
```

- `collisionMask` è un insieme di "tipi di suolo" percorribili: un tile blocca un'unità a terra
  se le due maschere **non hanno bit in comune** (media sul significato dei singoli bit).
- Le dimensioni sono box centrati (`size/2`), la quota è confrontata come intervallo verticale.
- Solo i vid `PresentOnGrid` partecipano (la query usa la griglia).
- `AskPlace` dello script (`0x413a24`) e la generazione dei figli dei veicoli usano `0x40a2d0`
  (stesso test per un nvid in un punto).

---

## 7. Armi

### 7.1 Tabella `Weapon` (9 byte, puntatore in `0x439d3c`, numero in `0x439d40`) (alta)

| off | campo | uso |
|---|---|---|
| 0 | targetCategory | maschera di `ObjectCategory` dei bersagli (4 = veicoli, 8 = aerei, 0x0c = entrambi, 0 = nessuno) |
| 1 | flags | 1 = spara anche se non allineato; 2 = torretta che ruota a vuoto quando inattiva; 4 = spara nella direzione attuale senza ruotare (in `Player::Tact` letto sull'arma del vid della **torretta**); 8 = in `FindTarget` tiene il candidato già trovato con probabilità 1/3. "Mira 80 px più in alto" (`attackZ = 80`) vale per le armi con `targetCategory & 8` (bersagli aerei: `0x40eadb`, `0x411c41`, `0x416bdb`; issue #13) |
| 2 | (sempre 0) | |
| 4 | range (s16) | portata, in distanza "di gioco" `max(|dx|, 1,5|dy|)` |
| 6 | scatter (u8) | dispersione ± sul punto d'impatto |
| 7 | cooldown (u16) | in **unità da 10 ms** (`timer = cooldown·10`) |

### 7.2 Scelta del bersaglio — `FindTarget` `0x40c970` (media)

Raggio = `vid.visibilityRadius`; candidati dalla lista unità della griglia con
`targetCategory & cat`, esercito ≠ 2 (neutrale) e ≠ il proprio, non `st_invisible`, distanza ≤
raggio. Sceglie il più vicino, con preferenze: se c'è già un candidato si tiene con probabilità
1/3 (`rand()%3`); gli aerei sono preferiti fra loro; per armi senza torretta il bersaglio deve
essere entro ±32 di direzione (±45°).

### 7.3 Sparare — `Fire(x,y,z)` `0x40e6b0` e `Aim` `0x40ada0` (alta)

```c
int Fire(x, y, z) {
    if (timer) { if (child && vid.linkedObjectVid) child->TurnTo(GetDirection(...)); return 0; }
    if (!Aim(x, y, z)) return 0;          // ruota (corpo o torretta) e fissa attackX/Y/Z
    timer = weapon.cooldown * 10;
    if (!vid.linkedObjectVid) ChangeAction(act_fight, -1);   // con torretta anima il figlio
    return 1;
}
```

`Aim` limita il punto alla portata (`range` lungo la direzione) e lo scrive in `attackX/Y/Z`
dello sparatore (o del figlio torretta, che passa ad `act_fight`).

### 7.4 Proiettili: figli dell'animazione di fuoco — `CreateChildren` `0x40a950` (alta)

I proiettili non sono creati dal codice dell'arma: sono i **figli dell'azione `act_fight`**
(`vid.childNvid[8]`, `childrenCount[8]`), creati a fine ciclo dell'animazione di fuoco:

```c
for (i = 0; i < childrenCount[action]; i++) {
    nv = |childNvid[action]|;
    dir = vids[nv].flags & RandomDirection ? rand() & 0xff : direction;
    if (childNvid[action] < 0) ox = offX - rand()%(2*offX+1), oy = offY - rand()%(2*offY+1);
    else ox,oy = offset ruotato con la direzione (i==0: (offX,offY); i==1: specchiato; i==2: solo avanti)
    if (vids[nv].class != Vehicle || posto libero)
        c = CreateUnit(nv, x+ox, y+oy, z+offZ[action], act_stand, dir, this);
    if (c) {
        if (action == act_fight) {
            sc = weapon.scatter;
            c->SetDestination(attackX + sc - rand()%(2sc+1) + ox',
                              attackY + sc - rand()%(2sc+1) + oy', attackZ);
        }
        c->Action(act_child, this, nv);
    }
}
```

Lo stesso meccanismo produce fumo, esplosioni, detriti, luci (figli di `act_death`,
`act_wound`, `act_birth`...).

Comportamento delle classi di proiettile (`act_nextcommand`):
- **Projectile (5)** `0x40f757`: all'arrivo (`state&3==3`) o a `z <= 0` → `act_death`;
  `act_init` lo mette subito in `act_death` (un proiettile senza destinazione esplode),
  `SetDestination` lo rimette in `act_go`. Non insegue: va al punto fissato allo sparo.
  `speedX = 9999` = raggio istantaneo fino al primo ostacolo (laser).
- **CannonMissile (6)** `0x410062`: esplode all'arrivo se `|goZ - z| < sizeZ + 20`; tiro
  balistico (4.3).
- **AviaMissile (15)** `0x4102c1`: missile **a ricerca**: a ogni rivalutazione `go = target`;
  colpisce entro un tile e `|dz| < sizeZ+20`; carburante `timer = 8000 ms` (`0x410466`).
- **Mine (13)** `0x411585`: cerca bersagli nel raggio di visibilità, esplode (`act_death`) entro
  `weapon.range`; se `behave & 4 == 0` si avvicina al bersaglio (mine mobili).
- **Shell (14)**, **Debris (16)**, **DownedAvia (7)**, **NA (8)**: cadono/rimbalzano e muoiono a
  terra o al blocco (media, non dettagliati).

### 7.5 Danno (alta)

- Diretto: `act_damage(n)` (sez. 3.4). Colpi singoli non esistono: il danno lo fa l'esplosione.
- **Ad area** alla fine dell'animazione di morte (`0x412580`, caso 15): se `vid.deathDamage`:
  per ogni sprite nel rettangolo `x ± (deathDamageRadius + halfX)`, `y ± (radius + halfY)` con
  `maxHP != 0`, se `|dx| < radius + o.halfX`, `|dy| < radius + o.halfY` e
  `|dz| < o.absSizeZ + 20` → `o.Action(act_damage, deathDamage, this)`. Nessuna attenuazione con
  la distanza. Vale per proiettili ed esplosioni come per le unità distrutte.
- Ferita: quando `hp` scende sotto metà → `act_wound` (i suoi figli sono fumo ecc.); se le unità
  tornano sopra metà (riparazione) il figlio di ferita (`childNvid[13]`) viene rimosso
  (`0x40c0e0`).
- Morte dei veicoli (`0x40eb62`): gli items di classe Bonus vengono lanciati in aria (`z+10`,
  direzione casuale, `vz = speedY`).

---

## 8. IA

### 8.1 `behave` (`+0x45`) (alta sull'uso, media sui nomi)

| bit | effetto |
|---|---|
| 1 | acquisisce automaticamente bersagli (`FindTarget`) |
| 2 | segue la squadra (torna verso il centro del gruppo se > 640 px); aviazione: decolla/resta in volo anche senza bersaglio |
| 4 | "evasivo": trova il bersaglio ma non lo attacca da solo; quando viene colpito si sposta di 64 px in direzione casuale; mine ferme |
| 0x80 | Building: non richiude il portone dopo aver fatto uscire un'unità |

Default: 1 (veicoli), 5 (aviazione). Lo script lo cambia con `act_setbehave`.

### 8.2 Veicoli — `act_nextcommand` `0x40ed35` (alta)

```c
if (action == death || action == unload) return;
if (speed < 0) { /* retromarcia */ if (allineato ±16 e 32 px liberi) { SetDestination(waypoint); return; } }
if ((state & 3) == 3) Stop();
if (waypoint) { if (entro un tile) ClearWaypoint(); else if (!(state & st_move)) SetDestination(waypoint); }
else ChangeAction(act_stand);
if (state & st_move) { d = turn ? direction + turn++/-- : GetDirection(go); if (speed < 0) d += 128;
                       if (!TurnTo(d)) frame = lastFrame + 1; else if (action != go) ChangeAction(go); }
if (vid.weapon && (behave & 1) && (state & 0xf0) in {0, st_move}) {
    target = FindTarget(); if (target && !(behave & 4)) Action(act_attack, target, 0);
}
if (target) {
    if (Distance(target) > weapon.range) {
        if ((state & 0x20) && !(state & st_move)) Approach(target.x, target.y);   // insegue
        else if (vid.linkedObjectVid) child->TurnTo(direction);                   // torretta avanti
    } else {
        if ((state & 0x20) && (state & st_move)) ClearWaypoint();
        Fire(target.x, target.y, target.z + sizeZ - sizeZ/4);
    }
} else if (state & 0x20) {                                  // act_coor_attack
    if (dist(attack) <= range) { if (Fire(attack)) { state &= ~0x20; Stop(); waypoint = none; } }
    else if (!(state & st_move)) Approach(attackX, attackY);
} else if (weapon.flags & 2) ruota la torretta di un passo;  // radar/torrette in attesa
else if (vid.linkedObjectVid) child->TurnTo(direction);
```

Le torrette fisse (`speedX = 0`, es. "M Automatic turrel") usano lo stesso codice: non si
muovono, ruotano e sparano.

### 8.3 Aviazione — `0x410782` (alta)

Se ha un bersaglio, ricarica a 0 e distanza ≤ portata: `act_fight`, `timer = cooldown·10`,
`attack = posizione del bersaglio` (i proiettili partono verso quel punto). Altrimenti vira verso
`go` o verso il bersaglio. In volo senza ordini: sonda 128 px avanti; se c'è un ostacolo vira a
destra, altrimenti con probabilità 1/5 a sinistra, 1/5 a destra, 3/5 prosegue (volo a cerchi). Quota: crociera 80; se 32 px davanti c'è un ostacolo sale o
scende di 20; sotto 50 risale, sopra 120 riscende. A terra decolla se ha un bersaglio o `behave&2`.

### 8.4 Fabbriche ed edifici (Building `0x410e10`) (alta)

- `act_buildunit(nvid)` (41): se `timer == 0`: `+0x54 = nvid`, `state |= 0x80`,
  `timer = buildTime·1000` ms.
- `act_nextcommand` (45) quando `timer` scade: crea l'unità con `CreateUnit(nvid, x, y, z0,
  act_unload, 128, 0)` (`z0 = sizeZ - 95` se esiste l'animazione di scarico, altrimenti 0) se il
  posto è libero; le **copia i comandi** dell'edificio fino ad `act_uncom` (punto di raccolta),
  le passa il `behave` (veicoli/aerei), la rende passiva e la fa uscire con la sequenza
  `open → unload → stand_open` (`act_calltact` anima la salita in quota). Uscita dall'ingombro
  dell'edificio → l'unità torna attiva (`goZ = 80`, `vz = speedY` per gli aerei) e l'edificio
  richiude (`close`, `load`) salvo `behave & 0x80`.
- Senza comandi né carico: sceglie **a caso** un elemento non-Bonus dei suoi items e lo
  costruisce (`act_buildunit`): produzione automatica.
- `act_uncom` (47) aggiunge il separatore fra comandi dell'edificio e comandi da copiare.
- `act_load_object`/`act_unload_object` (83/84): caricano/scaricano un'unità esistente (porte,
  hangar).

### 8.5 Eserciti, squadre, giocatori (media)

- La sezione Army della mappa (`0x41529c`) ha fino a 4 record giocatore (struttura da `0x255`
  byte a `field+0x10+i*0x255`): `a` = tipo (1 = controllato da un umano: solo questi ricevono
  input), `b` (non chiarito), `c` = indice di rete del giocatore, `flagman` (`+0x224`) = id
  dell'unità del giocatore. Ogni squadra diventa un **gruppo** (`0x4165f0`, lista `0x43f8e8`,
  25 byte: vettore membri, centro x/y, next).
- `Groups_Tact` `0x416730` a ogni frame: il target del gruppo è quello del primo membro che sta
  attaccando; ogni membro libero (`state&0xf0==0`) attacca quel target, oppure, se `behave & 2` e
  dista più di 640 dal centro, va (`act_move`) al centro ±100 px casuali. Il centro è la media
  "a dimezzamento" delle posizioni iniziali (`0x4166e0`), non viene aggiornato.
- Esercito 2 = neutrale (non viene preso di mira).
- Giocatore (`0x4169b0`): clic sinistro → `clearcom` + `act_move` del flagman; destro →
  `act_coor_attack`; la torretta segue il cursore; l'arma del flagman è scritta direttamente in
  `vid.weapon` del suo vid (`player+0x244`).

---

## 9. Camera e rendering

### 9.1 Camera (alta)

- `camX/camY` (`0x439cf4`/`0x439cf6`, s16) = angolo in alto a sinistra della vista in coordinate
  di mondo; `SetPosition(x,y)` la centra (`0x413ac3`), la mappa la imposta (`observerX/Y`).
- Scroll ai bordi (`0x415e8d`): mouse a ≤5 px dal bordo → velocità −40 per frame fino a −500
  (x) / −30 fino a −400 (y), simmetrico a destra/basso; altrove 0. `cam += v·dt >> 10` con dt
  reale (demo: `curclock`).
- Auto-scroll (`OPT_SCROLLING`): senza scroll manuale `v = flagman - centroVista - cam`
  (avvicinamento esponenziale al flagman).
- Guinzaglio: il flagman resta entro 586×440 px dal bordo della vista (`0x416004`).
- Clamp ai bordi della mappa con i margini della barra di stato (`0x42da0c..0x42da18`, regolabili
  con i tasti `+`/`-` a passi di 27×20 px).

### 9.2 Ordine di disegno (alta, salvo nota)

1. Strati `vid.z_layer` 0..15 in ordine (0 = più in fondo).
2. In ogni strato, ordine della lista: inserimento ordinato per **y crescente, poi z, poi x**
   (`0x40bd40`); lo strato 5 inserisce in testa senza ordinare (`0x40bcf0`).
3. **Gli sprite non vengono riordinati quando si muovono** (media-alta): la lista di riordino
   `0x439d0c` non viene mai riempita (unici 3 riferimenti nel codice di `0x40b880`). L'ordine è
   quello di creazione.
4. Per ogni sprite: i figli con `dz < 0` prima, poi lo sprite, poi i figli con `dz >= 0` (salvo
   flag 0x400 sul vid del figlio).
5. Culling: sprite oltre ±512 px dal centro vista (in x e in `y - z`) saltati.
6. Posizione: `sx = x - width/2 - camX`, `sy = y - height/2 - camY - z` (`0x40b620`, `0x4021f0`).

### 9.3 Ombre (alta)

`0x4021f0`: se `vid.flags & Shadow` e `z > 0`, prima del corpo disegna lo stesso fotogramma in
modalità ombra in `(sx - z/2 - z/4, sy + z/4 - z/2)` con z = 0 (ombra in basso a sinistra,
distanza proporzionale alla quota). Con `ZeroZ` la quota non sposta il disegno.

### 9.4 Palette e gamma (media)

Grafica a 16 bit (hicolor): ogni vid converte la propria palette RGB in 256 colori a 16 bit al
caricamento (`0x4055d0`). La gamma globale della mappa (`0x42df7b`, es. `gamma=0x777` negli
script) è applicata alla palette se diversa da `0x777` e il vid non ha `IgnoreGlobalGamma`
(`0x4072c0`): tre nibble per canale, 7 = neutro, 0 = azzera il canale, 15 = lo satura, 1–6
scurisce, 8–14 schiarisce. `st_gamma` sceglie una delle 4 palette alternative del vid
(`vid+0x132`, 512 byte l'una) copiandola nella palette attiva prima di disegnare.

### 9.5 Figli e sotto-oggetti (alta)

`childNvid/childrenOffsets/childrenCount` sono **generatori** per azione (sez. 7.4), i figli così
creati sono sprite indipendenti nelle liste. Il sotto-oggetto collegato (`linkedObjectVid`,
torrette) invece vive nella catena `child` del padre: segue `x+dx, y+dy, z+dz`, riceve il tick
prima del padre (dentro `SpriteTact` del padre) ed è disegnato con lui. `0x40b540` ruota gli
offset della catena quando il padre gira.

---

## 10. Generatore casuale (alta)

- `rand()` CRT MSVC `0x41c7b5`: `seed = seed*214013 + 2531011; return (seed >> 16) & 0x7fff;`
  stato a 32 bit in `0x42aebc` (valore iniziale 1). `srand` `0x41c7ab`.
- Seminato **solo al caricamento di ogni mappa** (`0x414c5f`):
  - demo in riproduzione: seed letto dalla demo (es. `demo1.dem`: `0x00910dc1`);
  - partita in rete: costante `0x8f`;
  - altrimenti `timeGetTime()`; in registrazione di demo il seed viene scritto nel file.
- Unico generatore per motore e script (`Random(max)` = `rand() % (max+1)`, `0x4130a4`). Usato da:
  jitter dei tick (`0x40bc63`, ogni frame), `CreateUnit` (`Randomized`), fase iniziale
  dell'animazione (`0x40c0b2`), `CreateChildren` (direzioni, offset, dispersione), `FindTarget`,
  IA dell'aviazione, dello scansare, delle squadre, scelta produzione delle fabbriche, editor di
  terreno, `Effect(eff_random)`, controllo di sincronia in rete.
- Per avere determinismo (save state, replay): salvare il seed a 32 bit insieme allo stato e
  rispettare **esattamente l'ordine delle chiamate**, che dipende dall'ordine delle liste e dal
  numero di frame (il jitter consuma un numero per sprite `Randomized` per frame).

---

## Per la issue #3

Fatti incontrati che riguardano script, input, salvataggi e demo:

- Gli "id" restituiti dalle builtin sono **puntatori** agli sprite (`Create` `0x412cc0` →
  `CreateUnit(nvid,x,y,z,0,0,0)`).
- Dispatcher delle builtin: `0x412b30`, `switch` su `id - 64` (tabella `0x413f5c`, id 64..139).
  Il ciclo dell'interprete è `0x41a790` (`main()` eseguito una volta per frame, prima del mondo).
- `GetTime()` (69) = `curclock` (tempo di gioco, si ferma nei rallentamenti e viene riportato
  indietro dopo `Pause`), non il tempo reale.
- `Random(max)` (90) = `rand() % (max+1)` sullo stesso RNG del motore.
- `Action(unit, act, v1, v2)` (67): `act > 15` → `unit->Action(act, v1, v2)` (restituisce il
  valore); `act <= 15` → `ChangeAction(act, -1)` e restituisce 0.
- `AddCommand` (87) inserisce in fondo alla pila (FIFO), rifiutato se la pila termina con
  `act_cyclecommand` (sez. 3.3).
- `SetWind(wind, direct)` (107): `0x439d54 = wind`, `0x429e14 = direct`.
- `Pause(flag)` (89) → `0x40ac70` (sez. 1.6) e resetta il cursore.
- `SetPosition(x, y)` (126): centra la camera.
- Demo (`.dem`): una sezione di tipo 7 con `u32 len, char name[len]` (mappa), `u32 seed`,
  `u32 curclock iniziale`, ~770 byte non analizzati, poi un record di 16 byte per frame
  `{u32 curclock; 12 byte di input}` (`0x4141f0`), in due segmenti che ripartono dallo stesso
  clock (non chiarito), e in coda il nome della demo successiva terminato da `0xffffffff`
  (`0x4149ef`). Il record di input è `+0x32dd` del campo: x/y del mouse nel mondo (s16), x/y
  sullo schermo (s16), pulsanti, tasto (`+0xa`).
- Il clock di gioco iniziale è `startTimer` della mappa (i salvataggi ripartono dal loro tempo).
- Struttura giocatore (`0x255` byte, 4 a `0x439d78`): tipo, indice di rete, flagman `+0x224`,
  oggetto sotto il cursore `+0x228`, arma selezionata `+0x244`.

---

## Non trovato / aperto

| argomento | stato | piste |
|---|---|---|
| pathfinding globale | non trovato | nessuna funzione di ricerca su griglia; vedi sez. 5 |
| riordino per y degli sprite in moto | non trovato (sembra assente) | `0x439d0c` mai popolata |
| significato dei singoli bit di `collisionMask` | aperto | dipende dai dati dei tile (`0x40a5c0`) |
| campo `b` dei record Army | aperto | lettura a `0x415335`, nessun uso trovato |
| Kassandra (armi multiple, munizioni `act_addammo/getammo`, `0x411a60`) | analizzato solo lo smistamento | casi 32/35/63/64 |
| Shell, Debris, DownedAvia, NA, Bonus, Image, Font, Effect1/2 | smistamento noto, comportamento non dettagliato | tabelle nella sez. 2.3 |
| `SetGamma` (builtin 106 → `0x4074a0`) | non analizzato oltre la conversione delle palette | `0x4072c0`, `0x406903` |
| blocco di ~770 byte all'inizio delle demo | aperto (issue #3) | |

## Indice delle funzioni principali

| indirizzo | nome proposto |
|---|---|
| 0x401550 | WinMain / main loop |
| 0x401f70 | SVGA::BeginFrame (clock) |
| 0x4141f0 | Field::Tact (frame) |
| 0x4149b0 | Field::LoadMap |
| 0x40b880 | World::Tact (disegno + scheduling) |
| 0x409a20 | SPRITE::Tact |
| 0x40bf30 | SPRITE::Init |
| 0x40e1b0 | CreateUnit |
| 0x40a950 | SPRITE::CreateChildren |
| 0x40b3e0 / 0x40b330 | ChangeAction / SetAnimation |
| 0x40d9e0 / 0x412580 | Action base sprite / materiale |
| 0x40ea20, 0x410e10, 0x4105a0, 0x40f710, 0x411a60 | Action di Vehicle, Building, Avia, Projectile, Kassandra |
| 0x409770 / 0x409950 / 0x40e780 | SetDestination / Stop / ClearWaypoint |
| 0x40e9a0 | Approach (waypoint) |
| 0x40b030 / 0x40aff0 | GetDirection (CORDIC) / Distance |
| 0x40b0b0 | TurnTo |
| 0x40a5c0 / 0x40a430 / 0x40a2d0 / 0x40a770 | CheckMove / AskPlace / AskPlaceVid / FirstObstacleOnLine |
| 0x40c970 | FindTarget |
| 0x40e6b0 / 0x40ada0 | Fire / Aim |
| 0x40c890 / 0x40c7c0 / 0x40c690 / 0x40c630 | Push comando (cima) / (fondo) / waypoint / rimozione |
| 0x40d580 / 0x40d6d0 / 0x40d7a0 / 0x40d870 | Grid: init / query / next / add |
| 0x416730 / 0x4165f0 | Groups::Tact / NewGroup |
| 0x4169b0 | Player::Tact |
| 0x415e20 | UI/scroll tact |
| 0x40b620 / 0x4021f0 | disegno sprite / putcadr (ombre) |
| 0x40ac70 | Pause |
| 0x41c7b5 / 0x41c7ab | rand / srand |
| 0x412b30 / 0x41a790 | builtin dispatcher / interprete script |
