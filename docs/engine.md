# Il motore di simulazione (issue #8)

Reimplementazione della simulazione di `gromada.exe` dietro il contratto `game::Engine`
(`src/game/engine_api.hpp`, vedi `docs/architecture.md`). La specifica è `docs/re/engine.md`; dove
il documento di reverse engineering era incompleto o impreciso il comportamento è stato riletto
nel disassemblato (indirizzi citati nel codice e qui sotto, §4).

## 1. File

| file | contenuto |
|---|---|
| `src/game/rng.hpp` | `MsvcRand`: `rand()` della CRT MSVC (`0x41c7b5`), unico per motore e script |
| `src/game/fixed_math.hpp` | funzioni pure: tabelle `Sin`/`NegCos`, CORDIC `getDirection` (`0x40b030`), distanza di gioco (`0x40aff0`), periodo del tick con jitter (`0x40bc55`), velocità, gravità, passo verticale e orizzontale con accumulatori a 7 bit (`0x409cbb..0x40a00e`), tabelle di rotazione (`0x40cf70`), indice di direzione, tabelle prospettiche (`0x414f38`) |
| `src/game/sprite.hpp` | `VidInfo` (il vid in memoria, modificabile come nell'originale: `SetVid`, `MaxUnit`, arma della Kassandra), `Sprite` (struttura `SPRITE` + estensioni delle classi), pila dei comandi |
| `src/game/world.hpp` | `World`: caricamento mappa (`0x4149b0`), liste per strato z, griglia spaziale (`0x431ab0`), `CreateUnit`/`Init`/`CreateChildren`, `ChangeAction`/`SetAnimation`, `SpriteTact` (`0x409a20`) e fisica, pila dei comandi, `SetDestination`, `TurnTo`, collisioni (`CheckMove`, `AskPlace`, `AskPlaceVid`, `FirstObstacleOnLine`), `FindTarget`, `Aim`, `Fire`, oggetti, giocatori e squadre |
| `src/game/world_actions.hpp` | le 21 funzioni `Action` (sprite base `0x40d9e0`, materiale `0x412580`, Vehicle `0x40ea20`, Building `0x410e10`, AviaVehicle `0x4105a0`, Projectile, CannonMissile, DownedAvia, NA, Image, Effect1, Effect2, Mine, Shell, AviaMissile, Debris, Kassandra `0x411a60`, Bonus, Font, RepairCannon) |
| `src/game/engine.hpp` | `GameEngine : Engine`: fasi del frame, giocatori (`0x4169b0`), camera (`0x415e20`, `0x414838`), disegno (`0x40b880`, `0x40b620`, `0x4021f0`), effetti, gamma, barra di stato (`0x416de0`), le 28 builtin |
| `src/game/hicolor.hpp` | il modello di colore a 16 bit dell'originale (issue #15): tabelle di `SVGA::SetMode` (`0x404ed0`), gamma (`0x4072c0`), percorsi dei pixel di `putcadr` per i formati 2/3/4/8 e passata d'ombra |
| `src/game/screen_effects.hpp` | `Effect()` (`0x406e40`) ed `EndFrame` (`0x406ea0`): i sei effetti, `eff_openwindow` compreso |
| `tools/engine_run.cpp` | strumento desktop: carica un livello, mette la Kassandra dove la mette lo script, simula N secondi a passo fisso con input scriptato, salva screenshot PPM e una traccia |
| `tests/engine_test.cpp`, `tests/engine_data_test.cpp` | test sintetici (CI) e con i dati del gioco (locali) |
| `tests/engine_oracle_test.cpp` | il codice del motore sui vettori dell'oracolo (33 famiglie, CI; `docs/oracle.md` §9) |

Tutto è header-only e va incluso una volta per binario (`engine.hpp` include `libgromada_all.hpp`).
Costruzione: `game::GameEngine engine{resources};` (`GameResources` di `fw.res`), poi `loadMap()`.

## 2. Uso dal lato sessione

Ordine di un frame, come in `Field::Tact` (`0x4141f0`):

```
engine.beginFrame(elapsedMs);   // curclock += min(elapsed, 71)
engine.setInput(record);        // record di 12 byte (controller o demo)
script main()                   // le builtin del motore passano da engine.callBuiltin()
engine.tactWorld();             // tick degli sprite scaduti
engine.draw(fb);                // in qualunque punto del frame (l'originale disegna prima del tick)
engine.tactPlayers();           // flagman: clic sinistro = muovi, destro = spara, torretta
engine.tactGroupAi();           // squadre
engine.updateCamera();          // scroll ai bordi, auto-scroll, guinzaglio, clamp
engine.takeSounds();            // SoundEvent con coordinate di mondo
```

Dopo aver disegnato menu, testi e barra di stato la sessione chiama `drawScreenEffects(fb)`
(effetti di `Effect()`, §8); subito dopo `tactPlayers()` legge `stateBarInfo()` (dati di
`StateBar::Tact` per la barra di stato, vedi `docs/session.md`). Se un frame non viene
disegnato, il passo degli effetti avviene comunque al `beginFrame` successivo.

- `camera().width/height` sono la dimensione della vista: la sessione li imposta (640×480 o
  16:9) e il motore li usa per culling, `SetPosition`, scroll e clamp.
- `ScrollLock(flag)` è una builtin della sessione ma agisce sulla camera del motore: per questo il
  contratto ha guadagnato `Engine::setScrollLock(bool)` (con implementazione vuota di default, quindi
  compatibile).
- Riproduzione delle demo: dopo `loadMap(mappa, seme della demo)` chiamare `setClock(curclock
  iniziale della demo)` (anche questo aggiunto al contratto con default vuoto), poi a ogni frame
  `beginFrame(delta registrato)` e `setInput(record registrato)`.
- `SetGraphDetail` (builtin della sessione) nell'originale scrive figli dei vid: la sessione può
  ottenerlo con `callBuiltin(127 /*SetVid*/, {nvid, VID_CHILD+k, valore})`.
- I tasti `1`..`4`, le munizioni, i bonus raccolti, la vittoria e la sconfitta sono gestiti dagli
  script (`HEADER.INI`, `Tact()`), che usano le azioni della Kassandra (`act_changeweapon`,
  `act_addammo`, `act_getammo`, `act_deleteitem`): il motore implementa quelle azioni.
- Gli handle restituiti agli script (`Create`, `Flagman`, `GetObject`, `FirstUnit`...) sono interi
  crescenti mai riusati nella stessa mappa (0 = nessuno).

## 3. Fedeltà per sottosistema

Colonna "oracolo": famiglie di `tests/data/oracle` verificate sul codice del motore (issue #12),
a 0 differenze salvo le divergenze di §5.

| sottosistema | stato |
|---|---|
| orologio, pausa, tick per sprite, jitter `Randomized`, consumo dell'RNG | fedele (pausa: D1; tempo negativo: D13); oracolo: `rand`, `clock`, `tick_schedule`, `sprite_timer`, `random_builtin` |
| matematica: direzione CORDIC, distanze, tabelle, vid derivati | fedele, aritmetica a 32 bit con riavvolgimento; oracolo: `direction`, `distance_*`, `sizeto`, `tables`, `rotation_tables`, `map_scale_tables`, `vid_derived` |
| liste per strato z, inserimento ordinato, puntatore "prossimo" salvato durante l'iterazione | fedele |
| `CreateUnit`, `Init` (sotto-oggetto collegato, fase casuale), figli per azione (`CreateChildren`, offset ruotati con la prospettiva, dispersione dell'arma) | fedele; oracolo: `create_children` (`CreateUnit` non coperta) |
| `SpriteTact`: suoni di ciclo, timer, figli, morte, orfani degli Effect2, fisica, comandi, `act_calltact`, catena dei figli | fedele |
| fisica: gravità, accelerazione/decelerazione, quota con arresto a `goZ`, moto con accumulatori, vento, raggio istantaneo (`speed 9999`), bit di arrivo, aggiornamento della griglia | fedele; oracolo: `physics`, `commands`, `change_action`, `turn_to`, `set_destination` |
| pila dei comandi: fondo/cima, `act_cyclecommand`, `act_submove`, waypoint con chiusura del giro, `GetCommands`/`SetCommands` | fedele |
| collisioni AABB con quota, terreno con maschere, schiacciamento, griglia con celle potenze di 2 | fedele; oracolo: `check_move`, `ask_place`, `first_obstacle`, `first_obstacle_grid` |
| armi: `FindTarget` (preferenze, 1/3 per l'antiaereo, settore per armi direzionali), `Aim`, `Fire`, proiettili come figli di `act_fight`, danno ad area, ferita/guarigione | fedele; oracolo: `find_target`, `aim_fire`, `damage`, `area_damage` |
| `Action` per classe | Projectile, Vehicle, Kassandra, Building: oracolo `action_*` (tutte le azioni gestite in proprio salvo quelle che creano unità o usano il cursore); le altre classi dalla lettura del disassemblato |
| IA: veicoli (waypoint, sterzata residua ±30, retromarcia, inseguimento, torrette, radar), aviazione (volo a cerchi, quota 50–120, decollo), fabbriche (produzione casuale, uscita, copia dei comandi, `act_uncom`), mine, missili a ricerca, squadre | fedele |
| Kassandra: input del giocatore in `act_nextcommand`, frecce, salto, cambio arma della torretta, munizioni, bonus, morte con munizioni lanciate | fedele |
| giocatore (`Player::Tact`): clic sinistro = `clearcom` + `act_move`, destro = `act_coor_attack`, torretta verso il mouse, unità sotto il cursore, tipo di cursore | fedele |
| camera: scroll ai bordi (accelerazione 40/30 per frame, limiti 500/400, tempo reale), auto-scroll, guinzaglio 586×440, clamp alla mappa | fedele (D8) |
| disegno: strati, ordinamento per frame dello strato 5, culling ±512, figli prima/dopo secondo `dz`, ombre, `ZeroZ`, coordinate di schermo, palette alternative | fedele, colore a 16 bit RGB565 bit per bit (§8); vedi D5 |
| effetti `Effect()`, gamma (`SetGamma`, `VID_GAMMA`) | fedeli bit per bit (§8); `eff_openwindow` cadenzato sul tempo (D6) |
| builtin del motore (28) | tutte; `CreateLight` memorizza le luci e non le disegna, come l'originale (D7) |
| `Save()` | scrive una `.map` V3 con oggetti, comandi e armate tramite `saveMap` di libgromada; i dettagli di `act_save` per classe non sono riprodotti |

## 4. Correzioni a `docs/re/engine.md` trovate nel disassemblato

- `CheckMove`/`AskPlace`/`AskPlaceVid`/`FirstObstacleOnLine` (`0x40a5c0` ...): la categoria deve
  essere **uguale** a 4, 8 o 16 (non una maschera di bit).
- `SpriteTact` (`0x40a116`): i comandi si eseguono a ogni tick se **l'azione** è < 6 (stand, build,
  go, start_move, rotazioni), non la classe; `act_nextcommand` si salta solo per le azioni go e
  rotazione finché i pixel percorsi sono meno di `rotationPeriod`.
- Ramo senza fisica (`0x40a22c`): a fine animazione `act_birth` passa ad `act_stand` (non "Shell").
- Lo strato 5 (testa `0x439d0c`) è la "lista di riordino": a ogni frame i suoi sprite visibili
  vengono inseriti ordinati per y, z, x in una lista temporanea solo per il disegno, poi la lista
  originale (non ordinata, in testa) è ripristinata; l'aggiornamento usa l'ordine non ordinato.
- `CreateUnit`: i figli di `act_birth` nascono subito solo se il vid **non** ha animazione di
  nascita; altrimenti nascono a fine animazione.
- L'input della Kassandra è letto in `act_nextcommand` della classe (`0x411db9`); `0x411a2c` è una
  tabella di salto.
- Danno ad area (`0x412825`): il rettangolo include anche la metà dell'ingombro di chi esplode.
- `CreateChildren`: il punto mirato riceve la dispersione prima in y poi in x (ordine delle
  chiamate a `rand`); `act_child` riceve il padre e l'**indice** del figlio.
- `FindTarget`: il vincolo di ±32 di direzione vale per le armi con flag 2.
- `act_changecoor` sposta solo gli sprite `PresentOnGrid`; `GetObject` cerca solo nella griglia.
- Le munizioni della Kassandra sono 8 `s16` da `+0x54`; i salti disponibili sono `ammo[4]`
  (`+0x5c`). Né il codice né gli script inviano `act_jump`.
- Le classi Image mettono `state |= 4` su sé e sulla catena (`act_init`): sono sempre di schermo.
- `TurnTo`: se lo sprite sta già ruotando nel verso opposto torna ad `act_go` invece di invertire.
- Effect2 `act_child` (`0x412a30`): si aggancia in fondo alla catena del padre; se il padre sta
  sparando genera subito i propri figli e muore.
- Trovate con l'oracolo (issue #12, `docs/oracle.md` §3): passo del vento (n. 3), punto impacchettato
  di `FirstObstacleOnLine` (17), antiaereo = `targetCategory & 8` (18) e arma della testa nella
  torretta del giocatore (19), ritorno di Projectile `act_nextcommand` (20), `GetDirection` che
  aggiorna `g_dist` anche in `FindTarget` e `Player::Tact` (24), `SizeTo` con il punto troncato a s16
  (14), aritmetica a 32 bit di `GetDirection`/`Distance(x, y)` con punti int32 (4, 5).

## 5. Divergenze volute dall'originale

- **D1 Pausa senza salto in avanti.** In `Pause(0)` l'originale sposta indietro `lastTact` di tutti
  gli sprite, che al tick successivo ricevono un `dt` lungo quanto la pausa. Qui solo gli sprite
  aggiornati durante la pausa (quelli di schermo) vengono riallineati; gli altri riprendono con il
  `dt` normale. L'orologio, come nell'originale, continua a correre in pausa (gli script misurano i
  tempi dei menu con `GetTime()`) e torna al valore salvato alla ripresa.
- **D2 Riferimenti come id.** `target`, flagman e "unità sotto il cursore" sono id e non puntatori:
  un bersaglio distrutto diventa "nessuno" invece di un puntatore pendente. La memoria di uno
  sprite distrutto viene riusata solo dal `tactWorld()` successivo.
- **D3 Liste doppiamente concatenate.** Solo per togliere uno sprite in O(1) (centinaia di
  distruzioni nello stesso frame costavano 7 ms sul livello 19); ordine e semantica del puntatore
  "prossimo" sono quelli dell'originale.
- **D4 Iteratori della griglia indipendenti.** L'originale ha un solo cursore globale; danno ad area
  e riparazione raccolgono prima i candidati, così le query annidate non si disturbano.
- **D5 Colore a 16 bit.** Il disegno riproduce le operazioni dell'originale su una superficie
  RGB565 (il formato più comune all'epoca; con una scheda 555 il verde dell'originale cambia di
  poco): la palette a 8 bit troncata (`0x4055d0`), la gamma a tre nibble (`0x4072c0`: 0 spegne il
  canale, 1–6 moltiplicano per n/7, 7 neutro, 8–14 schiariscono per 8/(15−n), 15 satura; il nibble
  rosso non è mascherato, sopra 15 il rosso resta invariato), le ombre che dimezzano ogni canale,
  la luce dei formati 3/4 (×8/(8−L) saturato), la trasparenza del formato 8
  ((src·op + dst·(7−op))/7 sui 5 bit alti). Il framebuffer resta RGBA8: un colore 565 è scritto
  espanso e riletto esattamente. Divergenze residue: (a) nel formato 4 una corsa con fattore blu 0
  e fattore rosso/verde non nullo passa nell'originale per `0x4021c0`, che sovrascrive la parte
  bassa del puntatore alla tabella con il pixel (`mov bx, ax`) e legge un valore che dipende
  dall'indirizzo dello heap: qui si applica lo schiarimento voluto; (b) i glifi dei testi della
  sessione (`ui::FontSet`) usano ancora la palette a 8 bit (differenza ≤ 7/255 per canale).
- **D6 Effetti `eff_*`.** Fedeli (sequenze, temporizzazione a 71 ms, operazioni sui pixel); gli
  effetti sopravvivono al caricamento di una mappa come nell'originale (`eff_fade` scurisce,
  `Load()`, la nuova mappa riappare). Solo `eff_openwindow` (la finestra si allarga di
  min(4, w·dt/1600) × min(3, h·dt/1600) px **per frame disegnato**) è cadenzato: un passo ogni
  37 ms di orologio di gioco (`ScreenEffects::ReferenceFrameMs`, il frame medio delle demo
  dell'originale: 33,7, 37,5 e 40,0 ms), un passo per frame registrato nelle demo.
- **D7 `CreateLight`.** Nessuna divergenza: `0x407530` aggiunge un record di 14 byte (x, y, z,
  colore, 90) a una lista (`SVGA+0x1fc9`) che nessuna funzione dell'eseguibile legge (verificato
  con la ricerca di tutti i riferimenti); nessuno script chiama `CreateLight`. Le luci restano
  memorizzate e non disegnate.
- **D8 Camera.** Scroll, guinzaglio e clamp sono tutti in `updateCamera()`; nell'originale lo scroll
  avviene prima dei giocatori. I margini della barra di stato e i tasti `+`/`-` (finestra di gioco
  ridotta) non sono gestiti: la vista è `camera().width × height`. Il guinzaglio (586×440 =
  vista 640×480 meno 54×40) su viste più larghe (16:9) mantiene lo stesso margine dal bordo
  (`larghezza − 54`, `altezza − 40`): identico in 640×480 (issue #16).
- **D9 Torretta del giocatore.** `Player::Tact` ruota la testa di un passo di direzione per
  chiamata (`0x40b220`), cioè per frame dell'originale. Qui i passi sono cadenzati sullo stesso
  frame di riferimento di D6: uno ogni 37 ms di orologio di gioco (al più 2 per frame), così la
  torretta gira alla stessa velocità a 60 e 120 fps e come l'originale a ~27 fps (verificato con
  `engine_run --step 37/16.667/8.333`: stessa direzione allo stesso istante). In riproduzione di
  una demo (`setClock`) resta un passo per frame registrato.
- **D10 Protezioni.** Dove l'originale legge memoria non valida si usa un valore neutro: catena dei
  figli vuota in `0x40c0e0`, direzione obsoleta in `Aim` con arma a flag 4 (troncata a un byte),
  indice d'arma fuori tabella nella Kassandra, `Random(-1)`, salto con velocità nulla.
- **D11 Selezione strategica e rete.** Selezione a rettangolo/lazo e ordini alle unità selezionate
  (`0x415e20`, parte dell'editor e della modalità strategica) non sono implementati: il giocatore
  comanda solo il flagman. In rete ogni flagman leggerebbe il proprio record (`World::setNetGame`),
  ma il motore riceve un solo input locale.
- **D12 Testo.** La classe Font è un segnaposto: i testi (`PrintText`, `Message`) sono della
  sessione.
- **D13 Tempo negativo.** `advanceClock(elapsedMs)` riceve un tempo di frame con segno: un valore
  negativo (non valido) non fa avanzare l'orologio. L'originale calcola `timeGetTime() − ultimo`
  senza segno: una differenza ≥ 2³¹ ms vale 71 ms. Per i tempi validi (0..2³¹−1) è identico.
- **D14 Statistiche delle morti.** Il colpo letale sulla Kassandra incrementa nell'originale un
  contatore nel record del giocatore (`0x41211a`, `docs/oracle.md` §3.22); il motore non tiene
  queste statistiche (nessuna builtin del motore le legge).
- **D15 Puntatori usati come coordinate.** Projectile `act_attack` calcola la direzione verso
  `(puntatore del bersaglio, v2)`: il motore usa `World::kPointerX` (`0x800000`, un indirizzo di
  heap tipico), che dà la stessa direzione dell'originale (circa est).
- Aggiunte a D10: `0x40c0e0` senza l'effetto di ferita nella catena, `act_coor_attack` della
  Kassandra senza testa o con una testa fuori dalle tabelle delle munizioni.

## 6. Verifica

- `tests/engine_test.cpp` (senza dati): RNG MSVC, CORDIC e distanze (valori di
  `tools/re/engine_reference.py`), velocità, gravità, moto, tabelle di rotazione, orologio,
  scheduling (un tick ogni 71 ms, consumo dell'RNG per frame), `MaxUnit` e fuori mappa, pila dei
  comandi (FIFO dello script, cima del motore, ciclo), movimento verso un waypoint, blocchi di
  unità e terreno, danno/ferita/guarigione, danno ad area, `FindTarget`, pausa, azioni di base.
- `tests/engine_data_test.cpp` (con `GROMADA_DATA`): tabelle `Sin`/`NegCos`/CORDIC confrontate con
  `gromada.exe`; tutti i 30 livelli per 60 s a 16,7 e 8,3 ms con la Kassandra dello script, un
  ordine di movimento e uno di fuoco; nelle 19 missioni vere (gli altri livelli sono mappe
  segnaposto da meno di 200 oggetti che gli script riempiono) i nemici si muovono e combattono, il
  flagman obbedisce; determinismo (stesso seme → stesso mondo); prestazioni del livello 19.
- `tests/engine_oracle_test.cpp` (senza dati, CI): il codice del motore su tutti i vettori
  dell'oracolo (`docs/oracle.md` §9) più un test di regressione della torretta del giocatore.
  Divergenze trovate e corrette (issue #12): passo del vento (1131 valori), `GetDirection` e
  `Distance(x, y)` con punti int32, `SizeTo`, `Random(INT_MAX)`, divisione per zero con `dirs = 0`
  nel caricamento dei vid (il motore andava in crash), punto impacchettato di
  `FirstObstacleOnLine`, Projectile `act_nextcommand`/`act_attack`, antiaereo di `act_coor_attack`
  e della torretta, `g_dist` di `FindTarget`.
- `tests/hicolor_test.cpp` (senza dati): tabelle, effetti, gamma e `putcadr` confrontati con
  l'originale (§8), sequenze di `EndFrame` e composizione di `eff_openwindow`.
- `tools/engine_run --data DIR --level 19 --seconds 60 --shots 1000,5000` per screenshot e traccia
  (`--gamma G`, `--effect ID@MS,...`, `ENGINE_TRACE_EFFECT=1`; la traccia riporta la direzione
  della torretta);
  opzioni `--step 8.333`, `--input "500:move:300,60;4000:fire:150,140:1500;7000:up:0,0:2000"`
  (coordinate relative al flagman), `--weapon 90` (testa iniziale), `--spawn x,y`,
  `--camera x,y --no-player`, `ENGINE_TRACE_DAMAGE=1` per stampare ogni `act_damage`.

Prestazioni misurate (desktop condiviso, build Release): livello 19 (15.300 sprite) tick del
mondo p99 ≈ 0,6 ms, massimo < 1 ms, disegno 640×480 ≈ 1 ms.

## 7. Per l'oracolo differenziale (issue #11)

Funzioni pure in `game::fx`: `getDirection`, `gameDistance`, `tickPeriod`, `integrateSpeed`,
`applyGravity`, `verticalStep`, `moveStep`, `rotateStep`, `directionIndex`, `Sin`, `NegCos`,
`PerspectiveTables`; RNG `game::MsvcRand`. Con un `World` costruito da vid sintetici
(`World{vids, weapons}` + `loadEmpty`) sono richiamabili isolatamente anche `checkMove`,
`askPlace`, `askPlaceVid`, `firstObstacleOnLine`, `findTarget`, `setDestination`, `turnTo`,
`createChildren`, `action(s, act_damage, ...)` e `spriteTact`. Per sostituire i collaboratori come
fa l'oracolo: `actionHook`/`changeActionHook`, e le versioni di `physics`, `firstObstacleOnLine` e
`createChildren` che ricevono le funzioni da chiamare (`docs/oracle.md` §9).

## 8. Colore a 16 bit ed effetti (issue #15)

L'originale disegna su una superficie DirectDraw a 16 bit; `SVGA::SetMode` (`0x404a80`, tabelle
`0x404ed0..0x40541e`) ricava dalle maschere RGB tutte le tabelle del renderer.
`src/game/hicolor.hpp` le riproduce come funzioni pure per RGB565:

| tabella (SVGA `0x42bfb8`) | contenuto | uso |
|---|---|---|
| `+0x2b` (64×2048 word), `+0x2f` (8×32) | schiarimento v·8/(8−L) saturato, L = 0..7 per canale | luce dei formati 3/4 (`0x405da0`, `0x402110`), lampi di `Effect`, gamma 8..14 |
| `+0x27` (6×2048) | scurimento v·k/7, k = 1..6 | gamma 1..6 |
| `+0x243` (6×1024 byte) | (src·op + dst·(7−op))/7 su 5 bit | formato 8 (trasparenza, `0x4032e2`) |
| `+6`, `+0x22f..+0x23f` | canali senza 1..5 bit bassi | ombre ((c & m)>>1), dissolvenza (`0x405c40`) |

`putcadr` (`0x4021f0`): con il flag `0x80` del vid e z > 0 disegna prima l'ombra proiettata
(`0x42df8c` = 1) in (x − z/2 − z/4, y + z/4 − z/2): il formato 2 dimezza la destinazione sotto i
pixel opachi e salta i propri pixel d'ombra; i formati 3 e 8 scuriscono con la dissolvenza di
livello (fattore >> 1) + 1; il formato 4 non controlla il flag e ridisegna la luce. Poi il corpo,
che salta i propri pixel d'ombra (comando `0x40`) se l'ombra proiettata è stata disegnata.

La palette di una grafica è convertita al caricamento (`0x406830`) con la gamma globale, a meno
che il vid **proprietario** della grafica (catena `+0x130`) abbia `IgnoreGlobalGamma`, per
qualunque formato. `SetGamma` (`0x4074a0`) applica la gamma in loco se la precedente era `0x777`,
altrimenti scarica le grafiche che si ricaricano con la nuova: in entrambi i casi la palette è
quella di base con la gamma corrente. Le palette alternative (`VID_GAMMA+k`, `0x406d60`) partono
dalla palette ricaricata (con la gamma globale) e ricevono la gamma `var`.

Effetti (`src/game/screen_effects.hpp`, applicati da `drawScreenEffects` all'immagine finita,
menu e barra di stato compresi, come `EndFrame` sulla superficie prima di presentarla):

| id | effetto | operazione | livelli (passo ogni 71 ms) |
|---|---|---|---|
| 1 | `eff_nuke` | schiarisce r, g, b di L (`0x405da0`) | 1, 3, 5, −32 (un frame), −30..−2 → (−L)>>2 |
| 2/3/4 | `eff_rnuke/gnuke/bnuke` | schiarisce un canale (`0x405e20`) | 1, 3, 5, −30 (un frame), −28..−2 |
| 5 | `eff_fade` | dissolvenza \|L\| (×15/16 .. ×1/32, 9 nero) | 1..9, −9 (un frame), −8..−1 |
| 6 | `eff_openwindow` | finestra che si apre dal centro sull'immagine precedente | rettangolo di ritaglio (`+0x1a54`) |
| 255 | `eff_random` | `rand() % 6 + 1` | |

Il timer dei passi (`0x42df90`) non si azzera con un nuovo effetto (il primo passo è di solito
immediato) né al caricamento di una mappa (con un orologio minore il confronto senza segno fa
un passo al primo frame). `Effect()` è ignorato mentre si apre la finestra; `Effect(0)` ferma
l'effetto in corso. Gli script usano 0, 1, 4 e 5.

**Verifica numerica** (`tools/oracle/gen_hicolor.py` → `tests/data/oracle/hicolor_*.json`,
`tests/hicolor_test.cpp`): il codice dell'originale eseguito in unicorn con una superficie
RGB565 produce maschere, tabelle (le 64 tabelle RG come hash), la conversione RGB → 16 bit
(1029 casi), i tre operatori di `Effect` su tutti i 65536 valori (531 casi), la gamma su una
palette per tutte le 4096 gamme 0..0xfff più 204 sopra (hash) e per intero per le 15 gamme degli
script, e `putcadr` su frame sintetici dei formati 2/3/4/8 con e senza ombra proiettata (40
superfici): tutto coincide bit per bit, salvo i pixel del percorso difettoso del formato 4 (D5).
