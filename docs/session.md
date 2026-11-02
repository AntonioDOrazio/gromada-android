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
   aggiornare: si vede lo stato del frame precedente); poi `engine.tactWorld()` e le animazioni
   degli oggetti di schermo (che nell'originale sono sprite del mondo) e la macchina da scrivere.
5. Aggiornamento dei menu (vedi sotto): se consuma il click, il record viene rimandato al motore
   con i bit azzerati prima di `tactPlayers()` (come `0x40c1d0`, che azzera i bit prima di
   `0x415e20`; prima, i tick di menu e testi come sprite di `World::Tact`); `tactPlayers()` e subito dopo la barra di stato (`StateBar::Tact`, chiamata
   dall'originale dentro `Player::Tact`), `tactGroupAi()`, `updateCamera()`; suoni (motore +
   `PlaySFX` + menu).
6. `engine.drawScreenEffects()` (effetti `Effect()` su tutta l'immagine, `docs/engine.md` §8),
   cursore.
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
- Zoom, risoluzione interna e movimento fluido (issue #20): la vista del motore sui livelli è
  `uscita / zoom` pixel di mondo, l'immagine è k volte la dimensione logica e il record mappa il
  cursore attraverso l'ultima immagine (`ScreenMapping`); i menu ricevono il cursore logico. Con
  1×, zoom 1 e movimento fluido spento tutto è come sopra. Vedi `docs/rendering.md`.

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
- **Animazioni** (verificate nel disassemblato, issue #15): gli oggetti di schermo sono sprite
  del mondo e si animano come `SpriteTact` (`0x409a20`) sull'orologio di gioco, anche in pausa
  (sprite di schermo): un fotogramma quando `orologio − lastTact ≥ frameDuration`, controllato
  una volta per frame, senza recupero (`lastTact = orologio`; a 60 fps un vid da 71 ms avanza
  ogni 5 frame, come gli sprite del motore). Fine di `act_birth` → `act_stand`; fine di
  `act_death` → l'oggetto sparisce; le altre azioni (luci dei pulsanti, indicatori della mappa
  delle missioni) ricominciano. `ChangeAction` (`0x40b3e0`) riarma sempre il suono (`state & 0x200`,
  azzerato anche se azione e direzione non cambiano) e, se cambiano, riparte dal primo
  fotogramma; il suono suona al primo tick successivo (le Image non lo riarmano a fine ciclo),
  **posizionale** rispetto al centro dello schermo degli script (pan `4·dx`, attenuazione
  `−2·max(|dx|,|dy|)`, `0x409ab4`), dopo i suoni del mondo; i testi (macchina da scrivere,
  `TextManager::tick`) allo stesso modo. Verificato sulle tre demo (issue #13). Il pulsante cliccato ha `lastTact −= 1000` (`0x40c2c4`): avanza al frame dopo. `Init`
  (`0x40bf30`) fa partire un'animazione di attesa da un fotogramma casuale (`rand()` condiviso
  con il motore; per le Image solo se ha più di 2 fotogrammi). `Pause(0)` riporta indietro
  l'orologio e i timer degli oggetti con lui. Il disegno usa il renderer a 16 bit del motore
  (`game/hicolor.hpp`: trasparenza esatta della barra di stato e del vetro dell'interfaccia).

## Testi e barra di stato

`PrintText`/`PrintFile` usano `ui::TextManager` (font, macchina da scrivere, suoni 23/24: vedi
`docs/ui.md`); `PrintFile` cerca nella cartella del gioco e poi in quella di salvataggio.
`StateBar(1)` carica `statebar.men` in una lista propria (non cliccabile) e abilita `Message`.
Tab (Select sul pad) la mostra e la nasconde (`HEADER.INI`: `opt_player ^= OPT_STATEBAR`).

Contenuto (`StateBar::Tact` `0x416de0`, verificato nel disassemblato, issue #15). L'originale la
esegue dentro `Player::Tact` del giocatore locale, quindi solo con un flagman e fuori dalla pausa;
la sessione la esegue subito dopo `tactPlayers()` con i dati di `Engine::stateBarInfo()`. Per
ogni oggetto della barra, in ordine di lista (`statebar.men`: 266, 61, 49):
- **266** (la barra, formato 8 con trasparenza, 5 direzioni), solo se il flagman ha una testa
  d'arma (sotto-oggetto): il nome del vid della testa e i 5 contatori di munizioni (`s16` a
  `+0x54`, il quinto sono i salti), scritti con il font 127 direttamente nell'immagine
  (`putcadr`, angolo in alto a sinistra, passo 7 px) a y = 266.y + 225 − 15: il nome da
  x = 266.x − 299, i numeri (`itoa`) allineati a destra in x = 266.x − 299 + 391 + 48·i; la
  direzione è l'indice dell'arma ricavato dal nvid della testa (`nvid−88` sotto 92, `nvid−202`
  sotto 207, 4 per 207, altrimenti `nvid−207`), convertito come `(i·256 + 256/dirs)/dirs`;
- **61** (radar, 32 direzioni): per ogni oggetto della griglia entro ±464 px dal flagman (escluso
  lui) un punto 2×2 in (radar + Δ/16) entro il rettangolo di ritaglio: giallo (200,200,50) per
  gli `AviaMissile`, aviazione ciano (56,145,165) dell'armata 1 o (35,88,100) delle altre, mine
  rosse (200,50,50), unità di terra verdi (83,165,54) dell'armata 1 o grigie (`0x7bef`) delle
  altre; la direzione punta all'unità più vicina dell'armata 1 (distanza ≤ 60000), 0 se nessuna;
- **49** (punti vita, 79 direzioni): direzione `hp · 127 / maxHP` del vid del flagman (metà
  delle direzioni), 0 se `maxHP` è 0.
`SetDirection` (`0x40b610`) riparte dal primo fotogramma: questi oggetti non si animano.
`Message(testo)` (`0x416cf0`) scrive con un'unità font creata da `StateBar(1)` e spostata in
(266.x − 299, 266.y + 225) = (21, 465): primo glifo in (21, 466); dopo 5 s (orologio di gioco)
dall'ultimo `Message` il testo torna vuoto. `Message` non è usata dagli script del gioco.

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

Comandi sul gioco vero (issue #16, `src/game/session_controls.hpp`, dettagli in `docs/controls.md`):
- `controlsFrame()` dà al core, prima di ogni frame, giocatore e bersagli in coordinate
  dell'immagine (`Engine::playerAim()`), gittata dell'arma, armi selezionabili (`weapon[k]` dello
  script + munizioni) e arma in uso (`nweapon`).
- Il record di input porta gli **ordini di movimento** dello schema diretto (`PcState::moveOrder`:
  click sinistro nel punto dell'ordine, senza tasto destro in quel frame) e tiene fuori dalla
  fascia di scorrimento ai bordi (6 px) il mouse "schermo" dei tocchi e del punto mirato
  (`PcState::edgeScroll`), lasciando esatto il punto nel mondo; nei menu non tocca nulla.
- `setCameraFollow(bool)`: prima di `updateCamera()` la vista segue il giocatore (zona morta,
  anticipo nella direzione di marcia); non in demo, nei menu e con `ScrollLock(1)`.
- Indicatore dell'arma (barra di stato nascosta): 2 s dopo un cambio o una pressione di `1`-`4`.

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
  nel descrittore *i*; 0x408dd0 accetta `1..N`, il suono 0 non è suonabile e `N` è lo slot
  di `PlayWAV`; mappatura in `game::effectSound`, verificata sulla tabella dell'originale). Fino alla correzione del 2/11/2026 il core suonava `fw.res[n−1]`: ogni effetto
  era quello accanto, e i loop (motori al minimo 40, teletrasporto 14) diventavano colpi ripetuti;
  a volume pieno e centrato. Suoni del motore (`takeSounds`, uno per ciclo d'animazione
  dell'azione) con pan `4·dx` e attenuazione `−2·max(|dx|,|dy|)` in centesimi di dB rispetto al
  centro della camera (0x409a20).
- Tutte le richieste passano per `game::SoundChannels` (`src/game/sound_channels.hpp`, issue
  #17), copia dello scheduler dell'originale (dettagli in `docs/re/runtime.md` §9): coda di 32
  richieste per frame (sotto −20 dB scartate; richieste uguali di effetti non ciclici fuse),
  **8 canali**, nessuna ripartenza di un effetto partito ≤ 50 ms prima, e a fine frame
  (`tact()`, dopo `updateCamera()`, con l'orologio di gioco) gli effetti ciclici (byte di tipo
  0 in `fw.res`, `SoundData::kind`: 14, 17, 29, 31 e i motori dei veicoli 40..46) suonano **in loop**: una
  richiesta di un effetto ciclico già in corso aggiorna solo volume e pan e lo tiene vivo; si
  fermano 500 ms dopo l'ultima richiesta. Se le richieste superano i canali liberi si scartano
  le più deboli. Tipo 100 = non posizionale (volume pieno). Prima della issue #17 ogni richiesta
  creava una nuova voce sovrapposta e il motore del veicolo ripartiva da capo a ogni tick
  (~50 ms): inviluppo periodico e "ronzio" in missione. Dalla issue #18 è una porta operazione
  per operazione, confrontata con l'originale in emulazione (`docs/oracle.md` §8): il sink
  (`SoundSink`) è un dispositivo DirectSound con un buffer per canale (`load`/`release`/`playing`/
  `setPan`/`setVolume`/`play`/`stop`, interi in centesimi di dB) e `Stop` conserva la posizione:
  un motore fermato dopo 500 ms che riparte sullo stesso canale continua da dove era, come
  nell'originale (prima ricominciava dall'inizio).
- `PlayWAV(file)` (0x408c50): carica il WAV nello slot `N`, lo accoda subito a volume pieno e lo
  suona **in loop** (escluso da `StopSFX()` e dalla scadenza); lo stesso file già in corso non
  riparte; `StopMusic()` lo ferma. Prima della #18 il WAV veniva caricato ma mai suonato.
- Il `Mixer` (`src/engine/audio.hpp`) dà a ogni voce un id (`playSfx` → id, `sfxPlaying`,
  `setSfxVolumePan`, `stopSfxVoice`, `pauseSfxVoice`/`resumeSfxVoice` per `Stop`/`Play`); il
  `MixerSink` della sessione traduce pan e attenuazione DirectSound in guadagni; le voci cicliche ripartono dal campione 0 senza
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
- `playerAim()` (issue #16): flagman, gittata dell'arma in uso e unità colpibili, per i comandi;
- `setScrollLock(bool)`: lo stato di `ScrollLock()` ferma lo scorrimento ai bordi, il guinzaglio
  e i tasti `+`/`-`, che sono del motore;
- `drawScreenEffects(FramebufferRef)`: gli effetti di `Effect()` valgono per tutta l'immagine,
  menu compresi, quindi vanno disegnati dopo i menu.
- `stateBarInfo()` (issue #15): i dati del mondo che `StateBar::Tact` legge (punti vita, testa
  d'arma, munizioni, punti del radar, nemico più vicino); `nullopt` senza flagman o in pausa.

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
  stop, pausa e ripresa). `resources_test` controlla il byte di tipo dei 47 effetti di `fw.res`.
- `sound_oracle_test` (sempre; con `GROMADA_DATA` anche la tabella di `fw.res`): 1355 sequenze di
  eventi con le chiamate DirectSound dell'originale, riprodotte esattamente (issue #18).
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

1. (Chiusa, issue #15) Animazioni degli oggetti di menu: verificate, vedi sopra.
2. (Chiusa, issue #15) Contenuto della barra di stato e posizione di `Message`: verificati.
3. Disegno: menu e testi sono disegnati sopra tutto il mondo; nell'originale sono negli stessi
   strati degli sprite (uno sprite del mondo di strato ≥ 10 coprirebbe la lente). Raro.
4. Ripetizione dei tasti: tempi predefiniti di Windows (ritardo 1, velocità ~15/s).
5. Rete: solo giocatore singolo.
