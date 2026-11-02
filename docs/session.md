# Sessione di gioco (issue #10)

La **sessione** fa girare il gioco originale: esegue il ciclo del frame di `gromada.exe`, gli
script `.INI` (logo, menu, scelta del giocatore, mappa delle missioni, livelli, vittoria,
crediti), i filmati, i salvataggi e le demo. Il mondo lo simula il **motore**, che la sessione usa
solo attraverso il contratto `game::Engine` (`src/game/engine_api.hpp`, `docs/architecture.md`).
Fonti: `docs/re/runtime.md` (script, builtin, menu, input, salvataggi, demo, audio/video) e
`docs/re/engine.md` §1 (ciclo principale), più il disassemblato per i punti indicati sotto.

## File

| File | Contenuto |
|---|---|
| `src/game/session.hpp` | `Session`: ciclo del frame, caricamento di mappe e script, tutte le builtin della sessione, audio, filmati, disegno di menu/testi/cursore, demo |
| `src/game/session_ui.hpp` | `SessionUi`: oggetti di menu, lista menu e suo aggiornamento (0x40c1d0), testi (macchina da scrivere), barra di stato |
| `src/game/session_input.hpp` | `InputRecordBuilder`: `input::PcState` → `OriginalInput` (record di 12 byte) |
| `src/game/session_files.hpp` | `SaveFiles`: `FOpen`/`FCreate`/`FRead`/`FWrite`/`FClose`/`FileCopy` nella cartella di salvataggio |
| `src/game/session_demo.hpp` | `DemoPlayer` e `DemoRecorder` (formato di `maps/demo*.dem`) |
| `src/game/session_movie.hpp` | `VideoMoviePlayer`: `PlayAVI` con `src/video` (solo nel core) |
| `src/game/viewer_engine.hpp` | `ViewerEngine`: **motore provvisorio** (vedi sotto) |

Tutti header-only. La sessione dipende dal contratto, non da un'implementazione del motore.

## Ciclo del frame

`Session::runFrame(pc, dt)` è chiamata a ogni `retro_run` con l'input del frame. Ordine
(`0x401550` → `0x4141f0`, come in `docs/architecture.md`):

1. `engine.beginFrame(ms)`: il tempo reale del frame in millisecondi interi (16/17 a 60 fps,
   8/8/9 a 120 fps: il resto si riporta), il motore limita a 71 ms.
2. `engine.setInput(record)`: il record del controller o della demo. `GetKey`, `GetInputState`,
   `GetNetKey`, `GetNetInputState` leggono lo stesso record.
3. `main()` dello script (una volta per frame, da capo).
4. `engine.draw()` e sopra menu, testi e barra di stato (l'originale disegna prima di
   aggiornare: si vede lo stato del frame precedente); poi `engine.tactWorld()`.
5. Aggiornamento dei menu (vedi sotto): se consuma il click, il record viene rimandato al motore
   con i bit azzerati prima di `tactPlayers()` (come `0x40c1d0`, che azzera i bit prima di
   `0x415e20`); animazioni dei menu e macchina da scrivere; `tactPlayers()`, `tactGroupAi()`,
   `updateCamera()`; suoni (motore + `PlaySFX` + menu).
6. `engine.drawScreenEffects()` (effetti `Effect()` su tutta l'immagine), cursore.
7. **Dopo il frame**, nell'ordine in cui lo script li ha chiesti (in originale sono
   `PostMessage`): `Load()`, `PlayAVI()`, `SetGraphMode()`, `Exit()`.

Durante un filmato lo script non gira (il ciclo dell'originale non chiama la tick); un tasto o un
click lo interrompono e quella pressione non arriva al gioco.

## Mappe, script e variabili

`Load("maps\\x.map")`: scarica la mappa (spariscono menu, testi e barra di stato, che
nell'originale sono sprite del mondo), `engine.loadMap(file, seme)`, `ScrollLock` torna a 0, si
compila `maps\x.ini` (percorsi senza distinzione di maiuscole) e si crea un nuovo interprete
(`iff` e stringhe ripartono da zero). Il seme è il tempo in millisecondi dall'avvio della sessione
(`timeGetTime()` nell'originale). Un file `.dem` avvia una demo.

**Le celle int sopravvivono al caricamento per posizione**, non per nome (§1.3): la sessione
ricostruisce la disposizione delle celle dell'originale (ogni globale, elementi degli array
compresi, in ordine di dichiarazione, parametri delle funzioni inclusi) e copia nel nuovo script
le celle int non coperte da un inizializzatore (anche gli elementi oltre un inizializzatore
parziale). Una cella che prima era stringa dà 0 (nell'originale un puntatore). La disposizione
è verificata sulle demo: le 157/146/146 celle registrate coincidono, con i valori iniziali al
posto giusto (`noammo`, `weapon`, `gamma`...).

L'interprete è configurato con `&&`/`||` senza cortocircuito (§1.5); le altre regole sono quelle
di `src/script` (issue #9).

## Schermi e coordinate

- **Vista**: ciò che disegna il motore = tutta l'uscita (640×480 o 854×480 in 16:9). La camera
  del motore ha la larghezza della vista. In riproduzione di una demo la vista è 640×480 centrata
  (le demo sono registrate a 640×480).
- **Schermo degli script**: 640×480, oppure 800×600 dopo `SetGraphMode(2)` (il `fw.ini` del gioco
  ha `opt_resolution=1`, quindi i menu usano i file `*8.men` e il centro (400,300)). Menu e testi
  sono disegnati **centrati nella vista**: in 800×600 su 640×480 l'origine è (−80,−60) e si vede
  esattamente la parte 640×480 centrale (la cornice nera vid 83 resta fuori); in 16:9 i menu
  640×480 sono centrati con il livello (o il nero) ai lati.
- **Barra di stato**: sempre `statebar.men` (640 di larghezza: la vista non è mai larga 800),
  ancorata al fondo della vista e centrata.
- Il record di input ha il mouse in coordinate della vista (`schermo + camera` = mondo); i menu
  usano `vista − origine dei menu`. In demo i menu usano le coordinate registrate.

## Builtin

Le 28 di `EngineBuiltinIds` vanno a `Engine::callBuiltin`, **tranne** `Action`, `GetX`, `GetY`,
`GetUnitVid` su un handle di un oggetto della sessione: gli script chiamano
`Action(menu,act_destroy,0,0)`, `Action(gamemenu,act_settextcount,0,0)`,
`Action(weapon_screen,act_changeaction,0,192)` su oggetti creati da `MenuCreate`/`PrintText`.
Gli handle della sessione hanno il bit 30 acceso (`0x40000000+n` oggetti, `0x48000000+n` testi):
**gli handle delle unità del motore devono restare sotto `0x40000000`**.

Le altre 48 sono della sessione:

| Gruppo | Builtin | Note |
|---|---|---|
| Sistema | `Load`, `PlayAVI`, `SetGraphMode`, `Exit` | dopo il frame; `Exit` → `RETRO_ENVIRONMENT_SHUTDOWN` (se il frontend non lo supporta il gioco riparte) |
| | `Print` (log), `strlen` (fino al primo NUL), `SetGraphDetail` | `SetGraphDetail` scrive i figli dei vid con `SetVid` del motore (0x412e8c) |
| | `SaveDemo` | registra la demo della prossima mappa (sotto) |
| | `E_InsertTerrain` | **stub**: funzione dell'editor di mappe |
| | `GetReg` | **stub**: `""` (nessun registro) |
| Input | `GetKey`, `GetInputState`, `SetCursor`, `ScrollLock` | `ScrollLock` passa anche al motore (`setScrollLock`) |
| Menu | `LoadMenu`, `FreeMenu`, `MenuCreate`, `MenuAction`, `MenuVid`, `MenuDir`, `MenuSelectVid` | `MenuCreate` di un vid non di classe 10 crea un'unità del motore (`Create`), come `CreateUnit` |
| Testi | `PrintText`, `PrintFile`, `DeleteText`, `Message`, `StateBar` | |
| Audio | `PlaySFX`, `StopSFX()`, `StopSFX(n)`, `PlayAudio`, `StopMusic`, `SetSound`, `SetMusic`, `PlayWAV` | |
| | `PlayMidi`, `mciSendString` | **stub**: ignorate (nessuno script le usa) |
| File | `FOpen`, `FCreate`, `FRead`, `FWrite`, `FClose`, `FileCopy` | cartella di salvataggio |
| Rete | `GetNetInfo` (giocatore 0, 0 giocatori), `GetNetKey`, `GetNetInputState` (= locali), `SetNet` (memorizzato) | giocatore singolo |
| | `Send`, `Receive` | **stub**: nessuna rete (`Receive` = 0) |

Stub: 7 (`E_InsertTerrain`, `GetReg`, `PlayMidi`, `mciSendString`, `Send`, `Receive`, e `SetNet`
che memorizza soltanto). Tutte le altre sono implementate secondo §3.

## Menu (lista menu e aggiornamento)

Dal disassemblato:
- **Lista menu** (`0x40d3f0`): gli oggetti di classe 10 (`Image`) creati da `MenuCreate`/
  `LoadMenu` entrano nella lista ordinata per `z_layer` del vid (`vid+0x46`) **decrescente**; un
  oggetto nuovo va prima del primo con strato ≤ al suo (i più recenti prima a parità).
- **Aggiornamento** (`0x40c1d0`), una volta per frame dopo il mondo, sul mouse in coordinate dello
  schermo degli script; per ogni oggetto in ordine di lista, saltando quelli con `vid.flags & 0x20`
  o azione 6 (`act_menudisable`) o 15:
  - fuori dal mouse: azione → `azione & 1` (stand giù/su);
  - sotto il mouse in `act_birth`: ignorato;
  - il primo sotto il mouse diventa il selezionato (`MenuSelectVid`, `MenuDir` = indice di
    direzione); gli altri sotto il mouse restano non selezionati;
  - click sinistro: `MenuVid` = nvid nel frame dopo, **tutti i bit dell'input azzerati** (il
    gioco non vede il click), azione → `act_menupressdown/up` (4/5), la scansione si ferma;
  - senza click: 0 → 2, 1 → 3 (selezione).
- **Hit test** (`0x40bec0`): `|mouse − centro| ≤ sizeX/2, sizeY/2` del vid, estremi compresi
  (`V_RECTANGLE` è 200×12 con una grafica 8×8).
- `MenuAction(nvid,dir,azione)` agisce sugli oggetti della lista con quel nvid e indice.
- **Disegno**: strati 0..15, dentro lo strato per y, z, x e ordine di creazione (ordine
  d'inserimento delle liste del mondo); i testi sono sprite dello strato del font (14).
- **Animazioni** (ipotesi ragionevole, non verificata): un fotogramma ogni `frameDuration` del vid
  anche in pausa (oggetti fissi allo schermo); un vid con animazione di nascita parte in
  `act_birth` e poi sta fermo; `act_death` rimuove l'oggetto a fine animazione (rumore dello
  schermo armi); il suono dell'azione suona una volta all'inizio dell'azione.

## Testi e barra di stato

`PrintText`/`PrintFile` usano `ui::TextManager` (font, macchina da scrivere, suoni 23/24: vedi
`docs/ui.md`); `PrintFile` cerca nella cartella del gioco e poi in quella di salvataggio.
`StateBar(1)` carica `statebar.men` in una lista propria (non cliccabile) e abilita `Message`.
Contenuto della barra (**ipotesi**, l'originale non è analizzato): la direzione della barra dei
punti vita (vid 49, 79 direzioni) è `hp · 78 / maxhp` (hp del flagman da `Action(act_gethp)`,
`maxhp` globale dello script), la direzione della barra (vid 266) è `nweapon`; `Message` scrive
in (24,452). `Message` non è usata dagli script del gioco.

## Input

`InputRecordBuilder` costruisce il record dell'originale (`docs/re/runtime.md` §6):
- mouse: posizione del cursore nella vista e nel mondo;
- bit: `LCLICK|LDOWN` alla pressione del sinistro, `LDOWN` finché tenuto (destro uguale), frecce
  tenute → 4/8/64/128 (Shift non c'è);
- tasto: Invio 13, Esc 27, Tab 9, Backspace 8, `1`..`4`, frecce 1..4, caratteri digitati;
  **un tasto per frame** (gli altri in coda) e **ripetizione automatica** dei tasti tenuti (0,5 s,
  poi 15 al secondo) come la tastiera di Windows, così tenendo premuta una freccia i menu scorrono.

Contesto per `src/input` (`Session::inputContext()`): `TextEntry` mentre `PLAYER.INI` chiede il
nome (`reset_name == 1`; il pad sceglie le lettere e la lettera in selezione lampeggia sul cursore
del font), `Menu` se ci sono oggetti nella lista menu o un filmato, altrimenti `Gameplay`. Quando il
contesto cambia mentre un pulsante è tenuto, ciò che è premuto viene ignorato fino al rilascio:
Start vale Invio mentre si scrive il nome ed Esc nei menu, e tenerlo premuto attraverso il cambio
annullerebbe la conferma.

Cursore: nascosto con `SetCursor(<0)`; freccia nei menu; altrimenti la forma di
`Engine::cursorAt()`; in demo è disegnato nella posizione registrata.

## Salvataggi

- Cartella: `<cartella di salvataggio di libretro>/gromada` (`RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY`).
  Al primo avvio vi si copiano dalla cartella del gioco `save1`..`save5` e `fw.ini` (solo se
  mancano). **Nella cartella del gioco non si scrive mai.**
- `FOpen` cerca nella cartella di salvataggio e poi in quella del gioco (senza distinzione di
  maiuscole); `FCreate`, `FWrite`, `FileCopy` scrivono solo nella cartella di salvataggio
  (anche un file del gioco aperto con `FOpen` e modificato viene scritto lì).
- Modo testo dell'originale: `FWrite` scrive `valore\r\n`; `FRead` legge una riga (al massimo 127
  byte, come `fgets(buf,128)`) senza `\r\n`; a fine file o con handle 0 restituisce la riga letta
  per ultima (buffer statico unico).
- Senza cartella di salvataggio i file scritti restano in memoria per la sessione.
- Nota: il `fw.ini` del gioco ha musica a 0 e 800×600: è l'originale; si cambia da *Opzioni*.

## Demo

- `Load("maps\\demoN.dem")` (dal menu principale dopo 30 s, o con l'opzione core *Demo*): carica la
  mappa registrata con il seme registrato (il suo `startTimer` coincide con l'orologio registrato),
  sovrascrive tutte le celle int dello script appena compilato con quelle registrate, e a ogni tick
  usa il record di input e il passo d'orologio registrati (`orologio registrato − orologio del
  motore`, 0..71). Come l'originale, una tick parte solo quando il tempo reale ha raggiunto il passo
  registrato: la demo va alla velocità della registrazione a 60 e a 120 fps.
- `FOpen`/`FCreate` restituiscono 0, `FRead` restituisce le righe registrate.
- Fine: un tasto o un click veri, la fine dei record o un `Load` chiesto dallo script caricano la
  mappa del trailer (`maps\mainmenu.map`).
- `SaveDemo(file)`: la prossima mappa caricata viene registrata (intestazione, celle int, per tick
  orologio + input + righe di `FRead`, marcatore di fine allo scaricamento) nel formato delle demo
  del gioco, nella cartella di salvataggio; il trailer non viene scritto (come l'exe).
- Le tre demo del gioco girano fino alla fine dei record con il motore provvisorio (la fedeltà del
  replay dipende dal motore vero).

## Audio e filmati

- `PlaySFX(n)`: effetto `n` = suono di indice `n` di `fw.res` (**da 0**: 0x408730 carica il suono *i*
  nel descrittore *i*; 0x408dd0 accetta `1..N`, il suono 0 è un segnaposto muto e `N` è lo slot
  di `PlayWAV`). Fino alla correzione del 2/11/2026 il core suonava `fw.res[n−1]`: ogni effetto
  era quello accanto, e i loop (motori al minimo 40, teletrasporto 14) diventavano colpi ripetuti;
  a volume pieno e centrato. Suoni del motore (`takeSounds`, uno per ciclo d'animazione
  dell'azione) con pan `4·dx` e attenuazione `−2·max(|dx|,|dy|)` in centesimi di dB rispetto al
  centro della camera (0x409a20).
- Tutte le richieste passano per `game::SoundChannels` (`src/game/sound_channels.hpp`, issue
  #17), copia dello scheduler dell'originale (dettagli in `docs/re/runtime.md` §9): coda di 32
  richieste per frame (sotto −20 dB scartate; richieste uguali di effetti non ciclici fuse),
  **8 canali**, nessuna ripartenza di un effetto partito ≤ 50 ms prima, e a fine frame
  (`tact()`, dopo `updateCamera()`, con l'orologio di gioco) gli effetti ciclici (byte di tipo
  0 in `fw.res`, `SoundData::kind`: motori dei veicoli 41..47, ecc.) suonano **in loop**: una
  richiesta di un effetto ciclico già in corso aggiorna solo volume e pan e lo tiene vivo; si
  fermano 500 ms dopo l'ultima richiesta. Se le richieste superano i canali liberi si scartano
  le più deboli. Tipo 100 = non posizionale (volume pieno). Prima della issue #17 ogni richiesta
  creava una nuova voce sovrapposta e il motore del veicolo ripartiva da capo a ogni tick
  (~50 ms): inviluppo periodico e "ronzio" in missione.
- Il `Mixer` (`src/engine/audio.hpp`) dà a ogni voce un id (`playSfx` → id, `sfxPlaying`,
  `setSfxVolumePan`, `stopSfxVoice`); le voci cicliche ripartono dal campione 0 senza
  discontinuità (interpolazione tra l'ultimo e il primo campione). `SessionConfig::log` riceve
  a livello Debug `sfx <n> loop start` a ogni avvio di un effetto ciclico (lo usa
  `tests/core_sfx_loop.sh`).
- `PlayAudio(t1..t4)`: playlist CD; negativi sostituiti da `t1`, `t1<0` non fa nulla, la stessa
  quaterna già in corso non riparte; traccia successiva con la regola dell'originale
  (`cur==t1 ? t2 : cur==t2 ? t3 : cur==t3 ? t4 : t1`, ora nel `Mixer`). `StopMusic` ferma il CD.
- `SetSound`/`SetMusic` 0–100 moltiplicano i volumi delle opzioni core; `SetMusic` da 0 a >0
  riavvia la playlist.
- `StopSFX()` ferma tutti i canali in corso tranne quello dell'ultimo numero di effetto (lo slot
  di `PlayWAV`), senza svuotare la coda del frame; `StopSFX(n)` (irraggiungibile dagli script)
  libera i canali di `n`; `StopMusic()` libera anche il canale dello slot `PlayWAV`.
  `PlayWAV(file)` carica un WAV (tipo 100) e restituisce il suo `nsfx` (l'originale lo suona
  subito in loop: non usato dagli script del gioco).
- `PlayAVI`: filmato a tutto schermo (centrato in 16:9), CD in pausa (musica muta) durante il
  filmato; se il file non si apre si continua senza.

## Motore provvisorio e sostituzione

`ViewerEngine` implementa `game::Engine` sopra i dati del viewer: oggetti della mappa (con i
sotto-oggetti collegati) come unità con handle stabili, animazioni, `Create`/`Action`/
`SetFlagman`/`GetObject`/`FirstUnit`/..., orologio (71 ms, `Pause` come 0x40ac70: l'orologio
corre e `Pause(0)` lo riporta indietro), `rand()` MSVC, camera (`SetPosition`, scorrimento ai
bordi di 0x415e20, auto-scroll, limiti della mappa), `SetGamma`, effetti di `Effect()` (dissolvenza,
lampi). Niente fisica, armi o IA: il flagman va verso il cursore col tasto sinistro tenuto (o con
le frecce), per guardare il livello.

**Sostituzione con il motore vero (#8)**: un solo punto, `makeGameEngine()` in
`src/gromada_libretro.cpp`:

    std::unique_ptr<gromada::game::Engine> makeGameEngine(const GameResources& resources) {
        return std::make_unique<gromada::game::ViewerEngine>(resources);   // ← motore vero qui
    }

I test della sessione usano un motore finto (`session_test`) o `ViewerEngine`
(`session_data_test`); al merge basta costruire lì il motore vero.

Aggiunte al contratto (compatibili: metodi virtuali con implementazione vuota):
- `setScrollLock(bool)`: lo stato di `ScrollLock()` ferma lo scorrimento ai bordi, il guinzaglio
  e i tasti `+`/`-`, che sono del motore;
- `drawScreenEffects(FramebufferRef)`: gli effetti di `Effect()` valgono per tutta l'immagine,
  menu compresi, quindi vanno disegnati dopo i menu.

## Core libretro

- La sessione sostituisce il visualizzatore. Opzioni nuove: **Modalità visualizzatore (debug)**
  (il viewer di prima con livello, scorrimento, prova suoni e filmati) e **Demo (debug)** (avvia
  `demoN.dem`). Restano 60/120 fps, 4:3/16:9, volumi e opzioni dei comandi.
- `retro_reset` riavvia la partita dal logo.

## Test

- `session_test` (sempre): record di input, file di salvataggio, lista e aggiornamento dei menu,
  testi, ordine del frame con un motore finto, builtin con script sintetici, celle int tra
  `Load()`, riproduzione e registrazione di demo, `SetGraphMode`/`Exit`.
- `session_data_test` (con `GROMADA_DATA`): logo → menu principale → nuovo giocatore (nome,
  difficoltà, `save2` scritto nella cartella di salvataggio) → mappa delle missioni → briefing del
  livello 1 (macchina da scrivere) → livello 1 → Esc → uscita → mappa delle missioni; le tre demo
  fino alla fine.
- `core_e2e`, `core_e2e_169_120fps` (con `GROMADA_DATA`): lo stesso percorso attraverso il core e
  l'harness (`tests/core_e2e.sh`), solo con il pad, con screenshot di ogni passo in
  `build/core_e2e_out` (non vanno nel repository). L'harness accetta ora `savedir=` e si ferma a
  `RETRO_ENVIRONMENT_SHUTDOWN`.
- `sound_channels_test` (sempre): scheduler degli effetti con un sink finto (loop tenuto vivo e
  fermato dopo 500 ms, guardia di 50 ms, 8 canali e scarto dei più deboli, formula di fusione,
  tipo 100, `StopSFX`) e voci cicliche del `Mixer` (giunzione senza discontinuità, volume/pan,
  stop). `resources_test` controlla il byte di tipo dei 47 effetti di `fw.res`.
- `core_sfx_loop`, `core_sfx_loop_120fps` (con `GROMADA_DATA`, `tests/core_sfx_loop.sh`): livello
  1 col veicolo (ELEPHANT) mosso da ordini di movimento per ~15 s; il motore (effetto 43) deve
  partire una volta sola. Misura (issue #17, livello 1 a 60/120 fps): prima 128/147 avvii del
  43 (uno per ciclo d'animazione), ora 1 avvio e 127/146 rinnovi; profondità dell'inviluppo
  mediato in sincrono al periodo del tick (50 / 41,7 ms) da 6,5 / 5,2 dB a 2,4 / 2,5 dB
  (controllo a un periodo non correlato: 1,3–2,4 dB).

## Save state

Lo stato completo della sessione (motore, script, UI, audio, input, demo, filmati) si salva e si
ricarica per i save state, il riavvolgimento e il run-ahead di RetroArch: `docs/savestates.md`.

## Ipotesi e domande aperte

1. Animazioni degli oggetti di menu e ritorno da `act_birth`/`act_menupress*` (sopra).
2. Contenuto della barra di stato e posizione di `Message`.
3. Disegno: menu e testi sono disegnati sopra tutto il mondo; nell'originale sono negli stessi
   strati degli sprite (uno sprite del mondo di strato ≥ 10 coprirebbe la lente). Raro.
4. Ripetizione dei tasti: tempi predefiniti di Windows (ritardo 1, velocità ~15/s).
5. Rete: solo giocatore singolo.
