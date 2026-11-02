# Save state (issue #14)

Il core supporta i save state di RetroArch: salvataggio e caricamento in qualunque momento,
riavvolgimento (*rewind*) e *run-ahead* (anche a istanza singola). Lo stato è **completo e
deterministico**: da uno stato caricato, con lo stesso input, il gioco produce gli stessi frame e
lo stesso audio (musica ed effetti compresi) dell'esecuzione originale, byte per byte.

## File

| File | Contenuto |
|---|---|
| `src/core/state_io.hpp` | `state::Writer`/`state::Reader` (campi little-endian a larghezza fissa, chunk `{tag, dimensione, dati}`, letture con controllo dei limiti che lanciano `state::Error`), `checksum64`, `crc32`, `fnv1a` |
| `src/game/savestate.hpp` | formato, mondo (`World`) e motore (`GameEngine`): codifica, decodifica con validazione, applicazione |
| `src/game/savestate_session.hpp` | chunk della sessione, intestazione, `SaveStateManager` (il punto d'ingresso), controlli sulle dimensioni |
| `src/script/vm_state.hpp` | interprete degli script (celle, memoria delle stringhe, codice patchato da `iff`) |
| `src/ui/text_state.hpp` | testi a schermo e macchina da scrivere |
| `src/input/mapper_state.hpp` | mapper dell'input (cursore, pulsanti, tocchi, accelerazione...) |
| `src/engine/audio_state.hpp` | mixer: musica (traccia e posizione esatta), voci degli effetti, sorgente in streaming (filmati) |
| `src/gromada_libretro.cpp` | `retro_serialize_size`/`retro_serialize`/`retro_unserialize` |

Ogni classe serializzata dichiara `friend struct SaveStateAccess;` (una riga): il codice dello
stato vive nei file sopra, non dentro le classi.

## Formato (versione 3)

Intestazione di 64 byte, poi i chunk:

| offset | campo |
|---|---|
| 0 | magic `GRMDSTAT` |
| 8 | versione (`SaveStateVersion` = 3; la 2 non aveva la macchina da scrivere a tempo di gioco della issue #13, la 1 lo scheduler dei suoni e gli effetti prima delle issue #15–#18: non si caricano più) |
| 12 | dimensione dell'intestazione (64) |
| 16 | dimensione del payload (u64) |
| 24 | checksum a 64 bit di intestazione (con questo campo a zero) e payload |
| 32 | impronta dei dati del gioco (`fw.res`: vid, nomi, classi, suoni, armi) |
| 40, 44 | larghezza e altezza dell'uscita al salvataggio (informative) |
| 48 | frame della sessione (informativo; l'harness lo usa per riprendere l'input) |
| 56, 60 | flag e riservato (devono essere 0) |

| chunk | contenuto |
|---|---|
| `CORE` | resto dei campioni audio del core (`audioDebt`, conta quanti campioni escono a ogni frame), chunk `INP2` del mapper dell'input (anche schema diretto: direzione di mira, ordini di movimento, richiesta d'arma in attesa) |
| `SESS` | sessione: mappa e storia, operazioni in coda (`Load`/`PlayAVI`/...), record di input, contesto e tasti "stantii", schermo degli script, orologi reali (`m_realMs` dà i semi di `Load`), contatori di frame e tick, demo in riproduzione (nome, posizione) e in registrazione (dati), filmato in corso (nome, orologio), scheduler degli effetti (`SoundChannels`: 32 richieste, 8 canali con buffer caricato, suono e tempo, file dell'ultimo `PlayWAV`) e i buffer DirectSound del `MixerSink` (suono, voce del mixer, pan, volume), file `PlayWAV` e slot N caricato, orologio del `Message()` della barra di stato, telecamera che segue il giocatore e indicatore dell'arma (issue #16), playlist CD e volumi degli script, `ScrollLock`, barra di stato, cursore, celle portate tra le mappe (`m_carry`), costruttore del record (coda dei tasti, ripetizione), file aperti dagli script (nome, contenuto, posizione), oggetti di menu (con `lastTact` e `SoundDone`: animano sull'orologio del gioco) e orologi dell'UI, lista menu, stato del menu, testi, identità dello script, immagine in uscita (solo durante una demo) |
| `SVM1` | interprete (se la mappa ha uno script): identità (hash del codice compilato, numero di celle e di `iff`), 2048 celle, blocchi della memoria emulata (codice patchato, buffer dei globali stringa, buffer statici di concatenazione e `_itoa`, risultati delle builtin), buffer di ogni cella stringa, contatori `iff`, stringhe internate, registri |
| `ENGN` | motore: **tutto il pool degli sprite** (anche gli slot morti) con i collegamenti come indici di slot (liste per strato, catene dei sotto-oggetti, `next`/`prev`), pile dei comandi (anche oltre `count`, l'originale ci rientra con `act_cyclecommand`), oggetti, liste libere nell'ordine, numero di id assegnati, celle della griglia **nel loro ordine** (l'ordine decide quale bersaglio si trova per primo), `gridUnits`, giocatori, squadre, suoni in attesa, orologio, pausa, RNG, vento, puntatore "orfano" degli Effect2, `m_lastDist`; i vid modificati a run time (`SetVid`, `MaxUnit`, conteggi, arma del flagman, palette alternative condivise), le armi (`VID_WEAPONRANGE`); intestazione della mappa, camera, scroll, `ScreenEffects` (effetto, livello, ultimo passo, rettangolo della finestra; le due immagini solo mentre è attivo `eff_openwindow`), gamma, cadenza della torretta (`m_turretClock`, `m_turretSteps`), `m_frameLocked`, `m_effectsDone`, luci, iteratori di `FirstUnit`/`NextUnit`, cache delle palette |
| `MIX1` | mixer: playlist, traccia corrente, posizione esatta (frame sorgente con la parte frazionaria), voci degli effetti (suono, posizione, guadagni, id, loop, pausa), prossimo id, posizione della colonna sonora del filmato |

La griglia non viene ricostruita ma salvata così com'è: ricostruirla cambierebbe l'ordine delle
celle e quindi le partite. Dei vid si salvano solo i record diversi da `fw.res` (più conteggi,
`MaxUnit` e arma, sempre), quindi lo stato resta piccolo.

**Non** fanno parte dello stato: i file su disco (`save1..5`, `fw.ini`, demo registrate: un file
scritto dopo il salvataggio resta scritto; i file *aperti* dagli script invece sì, con contenuto e
posizione), le opzioni del core (volumi, schema dei comandi, frame rate, formato), i file scritti
in memoria senza cartella di salvataggio (sono il "disco"), i nomi per variabile di `m_carry`
(usati solo dalla politica `ByName`, che la sessione non usa).

## Caricamento: validazione prima di toccare qualcosa

`SaveStateManager::load` controlla magic, versione, dimensioni, checksum e impronta dei dati, poi
decodifica **tutti** i chunk in valori "di appoggio" validati e solo alla fine li sposta nella
sessione: uno stato troncato, corrotto, di un'altra versione o di altri dati del gioco viene
rifiutato e la sessione resta esattamente com'era. La validazione copre ciò che proteggerebbe
la memoria: indici di slot e di vid nei limiti, liste per strato coerenti e senza cicli, catene dei
sotto-oggetti senza cicli, id unici e coerenti con il numero di sprite vivi, liste libere fatte di
slot morti, geometria della griglia, blocchi di memoria dello script non sovrapposti e con le
dimensioni giuste, conteggi con un limite (niente allocazioni enormi), valori in virgola mobile
finiti, nomi dei file aperti che restano relativi alla cartella di salvataggio. Valori che il gioco
stesso può produrre (azioni oltre 15, coordinate) non sono rifiutati.

Lo script: se quello in esecuzione ha la stessa identità si riusa; altrimenti (stato di un'altra
mappa) si compila lo `.ini` della mappa salvata (con una cache di 8 script compilati, così
riavvolgimento e run-ahead attraverso un `Load()` non ricompilano a ogni frame) e se l'identità
non coincide lo stato è rifiutato. Le demo si rileggono per nome (con cache), i suoni di
`PlayWAV` per nome.

## Musica, effetti, filmati

- **Musica**: se la stessa traccia è aperta e la posizione salvata è dentro (o dopo) la finestra
  già decodificata, cambia solo la posizione: il mixer ora tiene 8192 frame decodificati alle
  spalle, quindi riavvolgimento e run-ahead di pochi frame non decodificano niente. Altrimenti la
  traccia si riapre e il decodificatore cerca la posizione (`MusicStream::seek`: FLAC e WAV esatti,
  Vorbis verificato dai test sulla musica del gioco), o in mancanza decodifica dall'inizio.
- **Effetti**: le voci del mixer (anche quelle in loop dei motori e quelle in pausa, che
  riprendono da dove erano), lo scheduler dei canali dell'originale e i buffer per canale del
  `MixerSink` sono salvati interi: niente effetti persi né doppi, e l'audio dopo un caricamento è
  identico. Gli id delle voci restano quelli salvati: il mixer torna con le stesse voci e lo stesso
  prossimo id, quindi un buffer segue una voce ripristinata o una già finita (per il mixer "non in
  riproduzione", come nel gioco); nessun id può finire su una voce nuova.
- **Filmati**: lo stato registra il filmato e il suo orologio. Se si carica uno stato dello stesso
  filmato in corso l'orologio torna indietro e l'immagine resta quella già decodificata finché
  l'orologio non la raggiunge (i decodificatori non tornano indietro); se il filmato è un altro (o
  nessuno) si apre e si decodifica fino all'orologio salvato (esatto: il test carica in un altro
  processo uno stato salvato durante `1.avi` e ottiene gli stessi frame e lo stesso audio). La
  colonna sonora si riposiziona esattamente (`AudioStream::seek`). Con il run-ahead di un frame il
  risultato è esatto anche durante i filmati (verificato); RetroArch disattiva il run-ahead al
  primo `retro_serialize` fallito, per questo i filmati non rifiutano lo stato.
- **Demo**: lo stato contiene anche l'immagine in uscita, perché in una demo ci sono frame senza
  tick che ripresentano l'immagine precedente.

## Dimensione e tempi

`retro_serialize_size` deve restare costante per tutta la sessione: è un limite fisso,
`SaveStateMaxSize` = 8 MiB; lo stato occupa l'inizio del buffer e il resto è azzerato. Misure
(desktop condiviso, Release):

| situazione | dimensione | salvataggio | caricamento |
|---|---|---|---|
| livello 19 (15.400 sprite) | 1,78 MB | 1,0 ms | 1,5 ms |
| livello 1 | 0,21 MB | 0,5 ms (core, con l'azzeramento del buffer) | 0,15 ms |
| menu, mappa delle missioni | 45–50 KB | | |

Il riavvolgimento confronta stati di dimensione fissa con i campi a offset stabili, quindi i
delta restano piccoli. Prima il checksum era un CRC-32 byte per byte: 3,5 ms sul livello 19, da
qui il checksum a 64 bit.

## Test

- `savestate_test` (sempre): lettore/scrittore e checksum; VM (celle, `iff`, stringhe) salvata,
  eseguita, ricaricata e rieseguita, rifiuto di uno script diverso; una battaglia sintetica
  (carri con armi e proiettili, rocce, flagman, `SetVid`, `MaxUnit`, pausa, effetti, gamma):
  stessi byte di stato per 200 frame dopo il caricamento, anche in un altro motore, e con il
  run-ahead; 1500 chunk del motore corrotti (rifiutati o eseguiti senza crash); mapper; testi;
  mixer (musica da WAV attraverso la fine della traccia, voci in loop; caricamento nello stesso
  mixer e in uno nuovo).
- `savestate_data_test` (con `GROMADA_DATA`): la musica del CD (Ogg Vorbis) ripristinata in un
  mixer nuovo dopo 1, 97, 1234 e 4000 frame: zero campioni diversi; livello 19 e livello 1 (che
  con l'input scriptato finisce e torna alla mappa delle missioni) giocati con la musica accesa:
  400 frame dopo il caricamento identici (immagine + audio) e stato finale
  identico byte per byte, idempotenza (caricare e risalvare dà gli stessi byte), run-ahead per
  200 frame, tempi; uno stato caricato in un'altra sessione; una demo a metà (nella stessa
  sessione e in un'altra); robustezza: tutti i troncamenti fino a 4 KB e campionati oltre, bit
  invertiti, impronta e versione diverse, 3000 stati corrotti **con checksum ricalcolato** (la
  validazione strutturale li vede: circa 1800 rifiutati, gli altri girano 20 frame senza crash),
  e la sessione resta identica dopo ogni rifiuto.
- `savestate_e2e`, `savestate_e2e_169_120fps` (con `GROMADA_DATA`, `tests/savestate_e2e.sh`):
  attraverso il core e l'harness, lo stesso percorso di `core_e2e` (con la musica accesa nel
  `fw.ini` della cartella di salvataggio): livello 1, cambio di mappa,
  menu, filmato caricato in un altro processo, run-ahead su tutti i 1500 (3000) frame, lo schema
  di controllo diretto (telecamera che segue, mira assistita, ordini di movimento: ricaricamento
  e run-ahead), 400 stati corrotti rifiutati via `retro_unserialize`.

L'harness ha i comandi `savestate=F`, `loadstate=F`, `statefile=`, `runahead=N`, `hashes=` e
`statefuzz=N` (vedi l'intestazione di `tools/harness.cpp`).

## Aggiungere un campo

Ogni campo di una classe salvata che può cambiare durante il gioco deve finire nello stato.
Classi salvate: `World`, `Sprite`, `VidInfo`, `Player`, `Group`, `CommandStack`, `GameEngine`,
`ScreenEffects` (`src/game/savestate.hpp`); `Session`, `SessionUi`, `ScreenObject`,
`MenuState`, `SoundChannels`, `Session::MixerSink`, `SaveFiles`, `InputRecordBuilder`,
`DemoPlayer`, `DemoRecorder`, `FollowCamera`, `WeaponHud` (`src/game/savestate_session.hpp`);
`Interpreter`, `StringMemory` (`src/script/vm_state.hpp`); `TextManager`, `ScreenText`
(`src/ui/text_state.hpp`); `Mapper`, `PcState`, `RawInput` (`src/input/mapper_state.hpp`);
`Mixer`, `Mixer::Voice` (`src/engine/audio_state.hpp`).

Procedura (anche per chi fonde un branch e vede fallire la compilazione):

1. **Il controllo**: in fondo a `src/game/savestate_session.hpp` (e in `audio_state.hpp` per
   `Mixer::Voice`) ci sono `static_assert` sulla dimensione di ogni classe salvata, attivi su
   desktop x86-64 con libc++. Un campo nuovo ferma la compilazione con
   `save states: <classe> changed size`. (Un `bool` che finisce nel padding non cambia la
   dimensione: il controllo aiuta, non sostituisce la lettura del diff.)
2. **Decidere** se il campo è stato o no. Non si salvano: configurazione impostata dal core o
   dalle opzioni a ogni frame (es. `m_followOn`, `Mapper::m_settings`, `FollowCamera::Params`),
   funzioni/ganci di test o diagnostica (`World::actionHook`, `changeActionHook`,
   `damageHook`), cache derivabili ricostruite al volo, puntatori a dati statici. In quel caso
   basta aggiornare il numero nel `static_assert` e scrivere il motivo nel commento del campo.
3. **Salvarlo**: la classe ha già `friend struct SaveStateAccess;`. Nel file indicato sopra:
   scriverlo nella funzione di salvataggio (`saveWorld`/`saveEngine`/`saveSession`/`save`),
   leggerlo nella `decode*` corrispondente **nello stesso ordine**, validarlo se è un indice, una
   dimensione o un valore che indicizza memoria (`r.ranged`, `r.count` con limite,
   `finite32/64` per i float), metterlo nella struttura di appoggio (`ss::EngineState`,
   `SessionStaged`, ...) e copiarlo nella `apply*`. Puntatori a sprite: come indice di slot
   (`ref()` / `ptr()`); suoni: con `soundRef`/`lookup`; nessun puntatore grezzo nello stato.
4. **Versione**: alzare `SaveStateVersion` in `src/game/savestate.hpp` (gli stati vecchi
   vengono rifiutati con un messaggio, mai letti male).
5. **Dimensione**: aggiornare il numero nel `static_assert`.
6. **Verificare**: `savestate_test`, `savestate_data_test`, `savestate_e2e*` (con
   `GROMADA_DATA`). Se un campo manca, i test di determinismo trovano frame o byte di stato
   diversi dopo un caricamento; il test "salva, carica, risalva = stessi byte" trova i campi
   salvati ma non ripristinati.

## Limiti noti

- Il motore provvisorio `ViewerEngine` e la modalità visualizzatore non hanno save state
  (`retro_serialize` fallisce).
- Durante un filmato il riavvolgimento non può mostrare immagini passate del filmato (vedi sopra).
- `eff_openwindow` (che nessuno script del gioco usa) fotografa la schermata precedente quando
  parte: se parte nel primo frame dopo un caricamento fotografa l'ultima immagine disegnata prima
  del caricamento (l'immagine non è nello stato quando l'effetto non è attivo).
- I file su disco non tornano indietro con lo stato.
- Uno stato salvato a 16:9 si carica anche a 4:3 (e viceversa): la vista prende la dimensione
  attuale dal frame successivo; il determinismo vale a parità di opzioni.

## Campi della issue #19 (versione 4)

- Salvato: `Mapper::m_zoomArmed` (schema a puntamento: A/B premuti nel livello e ancora tenuti,
  cioè quelli che fanno zoom; un tasto tenuto dal menu non zooma).
- Non salvati, perché ricalcolati a ogni frame: `PcState::aimTarget`, `hasAimTarget`,
  `cursorHidden` (come `zoomRate`), i grilletti analogici `RawInput::l2/r2` (letti solo nel
  frame corrente, mai dall'input precedente), le nuove impostazioni del mapper (opzioni core).

## Campi della issue #13 (versione 3)

- Salvati: per ogni testo, lo stato della macchina da scrivere a tempo di gioco (`started`,
  `lastTact`, `timerMs`, `action`, `soundDone` di `ui::ScreenText`).
- Non salvati, perché non sono stato: gli agganci degli strumenti (`Session::builtinObserver`,
  `Session::m_sink`, `SoundChannels::requestObserver`) e i buffer riempiti e svuotati nello stesso
  frame (`m_screenSounds` di sessione, interfaccia e testi; `Session::m_soundCam`, la camera al
  momento del tick del mondo). Tra due frame sono vuoti o vengono riscritti prima dell'uso.
