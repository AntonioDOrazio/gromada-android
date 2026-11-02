# Oracolo a livello di partita: le demo dell'originale in emulazione (issue #13)

L'oracolo differenziale della issue #11 (`docs/oracle.md`) verifica funzioni isolate di
`gromada.exe`. Questo strumento verifica **la partita intera**: le tre demo registrate
(`maps/demo1..3.dem`) vengono rigiocate due volte, dal codice originale eseguito in emulazione e
dal nostro core (sessione + motore), e le due tracce vengono confrontate frame per frame:
mondo, generatore casuale, camera, chiamate builtin dello script e audio.

**Risultato: le tre demo coincidono con l'originale dall'inizio alla fine** (6379, 6792 e 5568
frame), compresi suoni e operazioni sui canali, dopo le correzioni elencate al §5.

| file | contenuto |
|---|---|
| `tools/oracle/trace_game.py` | l'originale in unicorn: avvio della CRT, `Field::Init`, `LoadMap` della demo, un `Field::Tact` per frame, campionamento |
| `tools/oracle/winenv.py` | l'ambiente Win32 minimo (heap, file, TEB/`fs`, CRT) |
| `tools/oracle/dsound.py` | DirectSound finto che registra le operazioni sui canali |
| `tools/game_trace.cpp` | la stessa traccia dal nostro core (`build/tests/game_trace`) |
| `tools/oracle/compare_traces.py` | confronto e rapporto |
| `tests/trace_oracle.sh` | test `trace_oracle` (ctest): 1500 frame di ogni demo, saltato senza dati o senza unicorn |

## 1. Uso

```sh
T=/percorso/fuori/dal/repo       # le tracce contengono dati del gioco: mai nel repository
/root/tools/oracle-venv/bin/python -I tools/oracle/trace_game.py 1 -o $T/demo1.orig.jsonl.gz
build/tests/game_trace --data $GROMADA_DATA --demo 1 -o $T/demo1.core.jsonl.gz
python3 -I tools/oracle/compare_traces.py $T/demo1.orig.jsonl.gz $T/demo1.core.jsonl.gz [--at F] [--json R]
# test automatico (primi 1500 frame delle tre demo):
GROMADA_DATA=... ORACLE_PYTHON=/root/tools/oracle-venv/bin/python ctest -R trace_oracle
```

Opzioni comuni: `--frames N` (solo i primi N frame), `--full-at F,F` (in quei frame anche gli
sprite statici). Tempi su questa macchina: originale 76 s / 31 s / 35 s per demo intera, core
meno di 10 s in tutto. Le tracce compresse pesano 10–105 MB.

## 2. Metodo: l'originale in emulazione

Si esegue `gromada.exe` vero, non una sua imitazione: la CRT statica, il caricatore di mappe e
script, il compilatore e la VM degli script, il motore, l'IA, il pianificatore dei suoni.

1. **Immagine e CRT.** L'immagine PE è mappata come nell'oracolo della #11. Si parte dal punto
   d'ingresso della CRT (`0x41d2bb`): heap, stdio, inizializzatori statici girano normalmente
   fino alla chiamata di `WinMain` (`0x401550`), che **non** viene eseguita (finestra, DirectDraw,
   DirectPlay). Le API di KERNEL32 usate dalla CRT sono emulate in `winenv.py`: heap
   deterministico (stessi indirizzi a ogni esecuzione), `VirtualAlloc`, file letti dalla cartella
   del gioco (percorsi senza distinzione di maiuscole), scritture solo in memoria (`error.log`),
   ambiente, code page, `fs:[0]` tramite una voce della GDT (unicorn ignora `FS_BASE` in modalità
   32 bit).
2. **Inizializzazione.** Al posto di `WinMain`: `hInstance`, dimensione dello schermo `640×480`
   (`0x42da00/04`) e rettangolo della vista `[0,639]×[0,479]` (`0x42da0c..18`), che normalmente
   imposta `SVGA::Init` (`0x404a00`, DirectDraw); poi `Field::Init` (`0x415af0`) vero: tabelle di
   rotazione, `fw.res` (vid, armi), suoni (`Sound::Init` con il DirectSound finto), cursori,
   giocatori.
3. **Demo.** `Field::LoadMap("maps\\demoN.dem")` (`0x4149b0`): l'originale legge l'intestazione
   della demo, carica la mappa con seme e orologio registrati, compila lo script e ripristina le
   celle intere registrate.
4. **Frame.** Un `Field::Tact(1)` (`0x4141f0`) per record: lettura del record (orologio + 12 byte
   d'input), `main()` dello script, `World::Tact`, menu, scroll, giocatori, squadre, clamp della
   camera, `Sound::Tact`. `timeGetTime` avanza di 1 s a ogni chiamata: l'attesa del tempo reale
   della demo non aspetta mai e `drawThisFrame` resta 0, quindi `World::Tact` non disegna
   (il disegno non ha effetti sulla simulazione). La demo finisce quando l'originale chiede `exit`
   (fine dei record) o un altro `Load` (`PostMessage` registrato).
5. **Campionamento** dopo ogni frame (sezione 3). Gli identificatori degli sprite sono l'ordine
   di creazione dalla mappa: un hook su `SPRITE::Init` (`0x40bf30`) numera ogni sprite (padre
   prima del figlio, come `allocSprite` del nostro motore); menu (classe 10) e testi (classe 19)
   sono esclusi perché nel nostro core sono oggetti della sessione. I puntatori passati agli
   script diventano lo stesso numero (`"S"` per gli altri oggetti del heap).

Prestazioni: **niente `count` in `emu_start`**. Unicorn implementa il limite di istruzioni con un
hook su ogni istruzione: l'emulazione scende da ~200 M a ~4 M istruzioni al secondo (vale anche
per `oracle.py`, che però emula tratti brevi). Gli hook di codice vanno installati prima che il
codice osservato venga tradotto la prima volta: unicorn non ritraduce i blocchi già in cache.

### Stub e sostituzioni

| cosa | come |
|---|---|
| `WinMain`, finestra, DirectDraw (`SVGA::Init`), DirectPlay, `CoInitialize` | non eseguiti; schermo 640×480 e rettangolo della vista scritti a mano |
| KERNEL32 (CRT, heap, file, ambiente, code page, `GetProcAddress`/`LoadLibraryA` → 0) | `winenv.py`; ogni altra API ferma l'emulazione (`UnexpectedCall`) |
| `timeGetTime` | +1000 ms per chiamata (nessuna attesa, nessun disegno) |
| `PostMessageA` | registrato: `Load` dello script e `exit` della demo chiudono la traccia |
| `mciSendStringA` (musica CD) | successo, risposta vuota |
| `LoadCursorFromFileA`, `LoadCursorA`, `DestroyCursor`, `SetCursor` | costanti |
| `waveOutGetNumDevs`, `auxGetNumDevs` | 0 (niente controllo del volume) |
| `DirectSoundCreate` (DSOUND #1) | `dsound.py`: IDirectSound/IDirectSoundBuffer finti |

Il DirectSound finto: `CreateSoundBuffer`/`DuplicateSoundBuffer` restituiscono buffer finti
(il duplicato di un buffer nullo è rifiutato come fa DirectSound: `0x408a2b` prova anche il canale
che sta allocando), `Lock` dà un'area di appoggio (la copia dei campioni gira davvero). Un buffer
"suona" per `byte·1000 / byteAlSecondo` ms di **orologio di gioco** dopo `Play` (i buffer in loop
fino a `Stop`): `game_trace` usa lo stesso modello per il nostro pianificatore (`TraceSink`),
così i due vedono gli stessi canali liberi. `SetPan` + `SetVolume` (+ `Play`) diventano una sola
operazione; si registrano solo gli `Stop` udibili (l'originale ripete `Stop` a ogni frame sui
canali inattivi che contengono un suono in loop: `0x409052` non controlla il flag attivo).

## 3. Formato della traccia

Una riga JSON per intestazione (`source`, `demo`, `fields`) e una per frame:

| chiave | contenuto |
|---|---|
| `f`, `clock`, `rng` | indice del record, `curclock`, stato di `rand()` (`0x42aebc`) |
| `n` | sprite nelle 16 liste (esclusi menu e testi) |
| `cam` | camera (`0x439cf4`); solo l'originale ha anche `rcam`, la camera **registrata** nella demo all'inizio del frame (mouse mondo − mouse schermo) |
| `crc` | CRC-32 per strato z su nvid, x, y, z, direzione, azione, fotogramma, stato di **ogni** sprite |
| `s` | sprite "dinamici" (tutti tranne terreno, statici, menu e testi) e le loro catene di figli: `uid, layer (−1 = figlio), cls, nvid, x, y, z, dir, act, frame, state, hp, speed, vz, goX, goY, goZ, timer, lastTact, ncmd, target, parent` |
| `b` | chiamate builtin dello script: `[id, [argomenti], risultato]` (risultato solo se spinto sulla pila) |
| `a` | audio: richieste `["r", nsfx, pan, attenuazione]` (`0x408dd0` / `SoundChannels::request`) e operazioni sui canali `["p", canale, nsfx, att, pan, loop]`, `["u", canale, nsfx, att, pan]` (loop tenuto vivo), `["s", canale, nsfx]` |
| `post` | mappa chiesta da `Load` nel frame |

`compare_traces.py` riporta per ogni aspetto il primo frame diverso con i dettagli (sprite,
campo, valori), le statistiche (frame con differenze, per campo; operazioni audio per effetto) e
confronta anche la camera di entrambi con quella registrata. Nell'ultimo frame, se entrambi
chiedono un `Load`, confronta solo le chiamate (l'originale campiona prima del caricamento, la
sessione dopo).

## 4. Risultati

| demo | mappa | frame emulati | builtin | richieste audio | operazioni canali | esito |
|---|---|---|---|---|---|---|
| demo1 | livello 10 | 6379 (tutta) | 274 177 | 20 173 | 11 564 | identica |
| demo2 | livello 06 | 6792 (tutta) | 239 197 | 14 876 | 9 070 | identica |
| demo3 | livello 14 | 5568 (tutta) | 178 678 | 18 591 | 6 736 | identica |

La camera dell'originale emulato coincide in ogni frame con quella registrata nelle demo: è una
verifica indipendente che l'emulazione riproduce la partita registrata.

Prima delle correzioni (primo frame divergente):

| demo | mondo | audio |
|---|---|---|
| demo1 | 428: ripresa dopo la pausa (D1) | 0: macchina da scrivere |
| demo2 | 1680: torretta del giocatore dopo un cambio d'arma | 0: macchina da scrivere |
| demo3 | 3163: torretta del giocatore | 0: macchina da scrivere |

## 5. Divergenze trovate e corrette

1. **Pausa** (`World::pause`, `0x40ac70`): alla ripresa ogni sprite riceve
   `lastTact += pauseStart − curclock`, anche sotto `pauseStart`. La divergenza voluta D1 limitava
   lo spostamento e ritardava il tick successivo: demo1 (che va in pausa) divergeva al frame 428.
   Il "salto in avanti" temuto non esiste: gli sprite ricevono il tick anche in pausa.
2. **Rettangolo della vista** (`clampCamera`, guinzaglio): è inclusivo, `x1 = 639`; la camera
   arriva a `mapW − 640` / `mapH − 480` (prima uno in meno). Visto sulla camera registrata.
3. **Torretta del giocatore** (`Player::Tact` `0x4169b0`): "torretta fissa in avanti" (flag 4) e
   "mira 80 px più in alto" vengono dall'arma del **vid della torretta**, non da quella del
   flagman; la seconda è `targetCategory & 8` (bersagli aerei), non `flags & 8`. Lo stesso test
   in `act_coor_attack` di Vehicle (`0x40eadb`) e Kassandra (`0x411c41`). `flags & 8` è invece la
   scelta 1/3 di `FindTarget` (già corretta).
4. **Proiettili** (`act_nextcommand` `0x40f757`): `act_go` solo quando `TurnTo` è allineato;
   durante la rotazione resta l'azione di rotazione.
5. **Suoni posizionali del mondo**: pan e attenuazione si calcolano con la camera del momento del
   tick (`0x409a20` dentro `World::Tact`), prima dello scroll e del clamp di fine frame.
6. **Testi e menu sono sprite**: la macchina da scrivere (`TextManager::tick`, `0x40e01c`) e gli
   oggetti di menu (`SessionUi::update` con l'orologio) ricevono il tick come gli sprite: al più
   uno per frame dopo `frameDuration` ms di orologio di gioco, mai nel frame della creazione;
   il suono di un'azione suona al tick **successivo** al cambio d'azione (anche se l'azione non
   cambia: `ChangeAction` azzera comunque il bit `0x200`), **posizionale** rispetto al centro dello
   schermo (prima: subito, al centro e a volume pieno, e la macchina da scrivere con il tempo
   reale accumulato), dopo i suoni del mondo; i tick precedono l'aggiornamento dei menu.

Test di regressione: `engine_test` (`testPause`, `testProjectileTurn`, `testPlayerTurret`, clamp
della camera in `testBuiltins`), `ui_text_test` (`testTypewriterTicks`), e `trace_oracle`.

Osservazioni sull'originale senza correzione necessaria:
- durante una demo `FOpen`/`FCreate` spingono 0 **senza togliere** l'argomento dalla pila della VM
  (`0x413782`, `0x41380a`): una cella in più fino alla fine di `main()`, senza effetti visibili;
- `Sound::Tact` richiama `Stop` a ogni frame sui canali inattivi con un suono in loop (sopra);
- in riproduzione `drawThisFrame` dipende dal tempo reale (frame saltati se il PC è lento); non
  cambia la simulazione.

## 6. Limiti

- Si verifica ciò che le demo esercitano: tre livelli (10, 06, 14), un giocatore, nessuna rete,
  niente menu principale né briefing fuori dalle demo, niente salvataggi.
- Il disegno non è eseguito (né confrontato): `docs/video.md`/test visivi restano il riferimento.
- Audio: si confrontano le richieste e le decisioni del pianificatore con lo stesso modello di
  durata a orologio di gioco. Il mixer reale (`Mixer`, tempo reale, ricampionamento, voci in loop)
  non è coperto: un difetto udibile che nasca lì (es. ripetizioni sul dispositivo) va cercato nel
  sink del mixer o nella relazione fra orologio di gioco e tempo audio, non nel pianificatore, che
  qui coincide con l'originale operazione per operazione.
- Lo stato completo degli sprite statici entra solo nei CRC per strato (`--full-at` per i
  dettagli); le estensioni delle classi (items, munizioni, waypoint) non sono nella traccia.
- `target` del nostro motore è un id (D2): nelle demo coincide, un puntatore pendente
  dell'originale non sarebbe confrontabile.
