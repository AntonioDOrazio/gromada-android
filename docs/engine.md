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
| `src/game/engine.hpp` | `GameEngine : Engine`: fasi del frame, giocatori (`0x4169b0`), camera (`0x415e20`, `0x414838`), disegno (`0x40b880`, `0x40b620`, `0x4021f0`), effetti, gamma, le 28 builtin |
| `tools/engine_run.cpp` | strumento desktop: carica un livello, mette la Kassandra dove la mette lo script, simula N secondi a passo fisso con input scriptato, salva screenshot PPM e una traccia |
| `tests/engine_test.cpp`, `tests/engine_data_test.cpp` | test sintetici (CI) e con i dati del gioco (locali) |

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

| sottosistema | stato |
|---|---|
| orologio, pausa, tick per sprite, jitter `Randomized`, consumo dell'RNG | fedele (verificato sulle tre demo, `docs/trace-oracle.md`) |
| liste per strato z, inserimento ordinato, puntatore "prossimo" salvato durante l'iterazione | fedele |
| `CreateUnit`, `Init` (sotto-oggetto collegato, fase casuale), figli per azione (`CreateChildren`, offset ruotati con la prospettiva, dispersione dell'arma) | fedele |
| `SpriteTact`: suoni di ciclo, timer, figli, morte, orfani degli Effect2, fisica, comandi, `act_calltact`, catena dei figli | fedele |
| fisica: gravità, accelerazione/decelerazione, quota con arresto a `goZ`, moto con accumulatori, vento, raggio istantaneo (`speed 9999`), bit di arrivo, aggiornamento della griglia | fedele |
| pila dei comandi: fondo/cima, `act_cyclecommand`, `act_submove`, waypoint con chiusura del giro, `GetCommands`/`SetCommands` | fedele |
| collisioni AABB con quota, terreno con maschere, schiacciamento, griglia con celle potenze di 2 | fedele |
| armi: `FindTarget` (preferenze, 1/3 per l'antiaereo, settore per armi direzionali), `Aim`, `Fire`, proiettili come figli di `act_fight`, danno ad area, ferita/guarigione | fedele |
| IA: veicoli (waypoint, sterzata residua ±30, retromarcia, inseguimento, torrette, radar), aviazione (volo a cerchi, quota 50–120, decollo), fabbriche (produzione casuale, uscita, copia dei comandi, `act_uncom`), mine, missili a ricerca, squadre | fedele |
| Kassandra: input del giocatore in `act_nextcommand`, frecce, salto, cambio arma della torretta, munizioni, bonus, morte con munizioni lanciate | fedele |
| giocatore (`Player::Tact`): clic sinistro = `clearcom` + `act_move`, destro = `act_coor_attack`, torretta verso il mouse, unità sotto il cursore, tipo di cursore | fedele |
| camera: scroll ai bordi (accelerazione 40/30 per frame, limiti 500/400, tempo reale), auto-scroll, guinzaglio 586×440, clamp alla mappa | fedele (D8) |
| disegno: strati, ordinamento per frame dello strato 5, culling ±512, figli prima/dopo secondo `dz`, ombre, `ZeroZ`, coordinate di schermo, palette alternative | fedele nell'ordine; resa del colore approssimata (D5, D6) |
| builtin del motore (28) | tutte; `CreateLight` memorizza le luci senza disegnarle (D7), `Effect` approssimato (D6) |
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

## 5. Divergenze volute dall'originale

- ~~D1 Pausa senza salto in avanti~~ (rimossa, issue #13): gli sprite ricevono il tick anche in
  pausa (salta solo fisica e comandi), quindi `Pause(0)` dell'originale (`lastTact += pauseStart -
  curclock` per tutti) non provoca nessun salto. Ora è riprodotto alla lettera: la versione con il
  limite a `pauseStart` faceva divergere demo1 alla ripresa dalla pausa.
- **D2 Riferimenti come id.** `target`, flagman e "unità sotto il cursore" sono id e non puntatori:
  un bersaglio distrutto diventa "nessuno" invece di un puntatore pendente. La memoria di uno
  sprite distrutto viene riusata solo dal `tactWorld()` successivo.
- **D3 Liste doppiamente concatenate.** Solo per togliere uno sprite in O(1) (centinaia di
  distruzioni nello stesso frame costavano 7 ms sul livello 19); ordine e semantica del puntatore
  "prossimo" sono quelli dell'originale.
- **D4 Iteratori della griglia indipendenti.** L'originale ha un solo cursore globale; danno ad area
  e riparazione raccolgono prima i candidati, così le query annidate non si disturbano.
- **D5 Gamma e ombre.** La gamma della mappa (`SetGamma`) e le palette alternative (`SetVid
  VID_GAMMA`) seguono la regola dei tre nibble (7 neutro, 0 spento, 15 saturo) applicata a canali
  a 8 bit; l'originale usa tabelle sulla palette a 16 bit non analizzate. L'ombra dimezza i colori
  di destinazione.
- **D6 Effetti `eff_*`.** Lampo bianco/rosso/verde/blu e dissolvenza seguono la cadenza
  dell'originale (+2 ogni 71 ms fino a 7, poi rientro da −32) con una miscela lineare verso il
  colore; `eff_openwindow` (transizione dalla schermata precedente) non è riprodotto.
- **D7 `CreateLight`.** Le luci sono memorizzate ma non disegnate (renderer `0x407530` non
  analizzato).
- **D8 Camera.** Scroll, guinzaglio e clamp sono tutti in `updateCamera()`; nell'originale lo scroll
  avviene prima dei giocatori. I margini della barra di stato e i tasti `+`/`-` (finestra di gioco
  ridotta) non sono gestiti: la vista è il rettangolo inclusivo `[0, width-1] × [0, height-1]`
  (`0x42da0c..0x42da18`), quindi la camera arriva a `mapW - width` (verificato sulle demo).
- **D9 Torretta del giocatore.** Come nell'originale ruota di un passo per frame disegnato
  (`0x40b220` in `Player::Tact`): a 60/120 fps gira più in fretta che a 25–35 fps. Non è stato
  normalizzato per non cambiare il ritmo del gioco senza un riferimento.
- **D10 Protezioni.** Dove l'originale legge memoria non valida si usa un valore neutro: catena dei
  figli vuota in `0x40c0e0`, direzione obsoleta in `Aim` con arma a flag 4 (troncata a un byte),
  indice d'arma fuori tabella nella Kassandra, `Random(-1)`, salto con velocità nulla.
- **D11 Selezione strategica e rete.** Selezione a rettangolo/lazo e ordini alle unità selezionate
  (`0x415e20`, parte dell'editor e della modalità strategica) non sono implementati: il giocatore
  comanda solo il flagman. In rete ogni flagman leggerebbe il proprio record (`World::setNetGame`),
  ma il motore riceve un solo input locale.
- **D12 Testo.** La classe Font è un segnaposto: i testi (`PrintText`, `Message`) sono della
  sessione.

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
- `tools/engine_run --data DIR --level 19 --seconds 60 --shots 1000,5000` per screenshot e traccia;
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
`createChildren`, `action(s, act_damage, ...)` e `spriteTact`.
