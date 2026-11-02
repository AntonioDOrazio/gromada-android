# Architettura del gioco (ondata 2)

Obiettivo dell'ondata 2: il core passa da visualizzatore a **gioco giocabile**. I pezzi sono
sviluppati in parallelo attorno a un contratto fisso, `src/game/engine_api.hpp`.

## Moduli e responsabilità

| Modulo | Issue | Contenuto |
|---|---|---|
| `src/game/engine*` (motore) | #8 | Simulazione fedele a `docs/re/engine.md`: orologio, tick per sprite, 21 classi, azioni, pila dei comandi, fisica, collisioni, armi, IA, camera, disegno, RNG; controllo del giocatore dal record di input; le builtin "del mondo". Implementa `game::Engine`. |
| `src/script` (interprete) | #9 | Allineamento alla semantica dell'originale (`docs/re/runtime.md` §1–2). |
| `src/game/session*` (sessione) | #10 | Ciclo del frame nell'ordine originale, stati (logo, filmati, menu, briefing, livello, vittoria/sconfitta), caricamento mappe e script, builtin di menu/testo/audio/file/sistema, conversione `input::PcState` → `OriginalInput`, cursore, barra di stato, salvataggi `save1..5` e `fw.ini`, riproduzione delle demo, integrazione nel core libretro. |
| `tests/oracle`, `tools/oracle` | #11 | Oracolo differenziale: funzioni originali di `gromada.exe` eseguite in emulazione (unicorn) per produrre vettori di test che verificano il motore. |
| `tools/oracle/trace_game.py`, `tools/game_trace.cpp` | #13 | Oracolo a livello di partita: le tre demo rigiocate dall'originale in emulazione e dal core, confronto frame per frame di mondo, RNG, camera, builtin e audio (`docs/trace-oracle.md`). |

Moduli già esistenti e riusati: `libgromada` (formati), `engine/audio.hpp` (mixer), `ui/` (font,
testi, menu), `input/` (controller, cursori), `video/` (filmati), `demo/` (parser `.dem`).

## Il contratto `game::Engine`

- La sessione possiede l'interprete e il mixer; il motore possiede il mondo e la camera.
- Ogni chiamata builtin dello script passa dalla sessione: se `isEngineBuiltin(id)` va a
  `Engine::callBuiltin`, altrimenti la gestisce la sessione.
- L'input arriva al motore **solo** come `OriginalInput` (il record di 12 byte dell'originale),
  sia dal controller sia dalle demo. Così le demo pilotano il gioco esattamente come i comandi
  veri.
- Eccezione (issue #10): `Action`, `GetX`, `GetY`, `GetUnitVid` su un handle di un oggetto della
  sessione (menu, testi: bit 30 acceso, `>= 0x40000000`) li gestisce la sessione; gli handle delle
  unità del motore devono restare sotto `0x40000000`.
- Query opzionale aggiunta dalla #16 (controlli): `playerAim()` (flagman, gittata dell'arma,
  unità colpibili; vuota di default), vedi `docs/controls.md`.
- Ganci opzionali aggiunti dalla #10 (implementazione vuota di default): `setScrollLock(bool)` e
  `drawScreenEffects(fb)`; vedi `docs/session.md`.
- Gancio opzionale aggiunto dalla #15: `stateBarInfo()` (dati della barra di stato).
- Il motore si costruisce in un solo punto: `makeGameEngine()` in `src/gromada_libretro.cpp`.
- Finché il motore (#8) non è fuso, la sessione usa un'implementazione provvisoria del contratto
  basata su `engine/level.hpp` (disegno del livello, builtin minime), così da poter sviluppare menu,
  briefing e flusso di gioco in parallelo.

## Ordine del frame (dall'originale, `docs/re/engine.md` §1.3)

1. `beginFrame(elapsed)`: l'orologio avanza di `min(elapsed, 71)` ms (fermo in pausa).
2. Input: `setInput(record)` (controller o demo); `GetKey`/`GetInputState` leggono lo stesso record.
3. Script: `main()` del livello (l'interprete chiama le builtin).
4. Mondo: `tactWorld()`; disegno con `draw()` (l'originale disegna prima di aggiornare).
5. UI e scroll; `tactPlayers()`; `tactGroupAi()`; `updateCamera()`; suoni (`takeSounds()`).
6. I `Load()`/`PlayAVI()` chiesti dallo script si eseguono **dopo** la fine del frame.

## Tempo e frame rate

Il core gira a 60 o 120 fps. Il passo dell'orologio di gioco per frame è il tempo reale del frame
(16,7 o 8,3 ms): l'originale non ha un tick globale, ogni sprite si aggiorna quando è scaduto il suo
`frameDuration`, quindi la simulazione resta corretta a entrambi i frame rate. In riproduzione demo
si usano invece i passi registrati nella demo.

## Ripartizione delle builtin

- **Motore** (28): `SizeTo`, `Action`, `Create`, `GetTime`, `MaxUnit`, `Effect`, `Flagman`,
  `GetObject`, `AddCommand`, `Pause`, `Random`, `GetUnitVid`, `FirstUnit`, `NextUnit`, `SetGamma`,
  `SetWind`, `CreateLight`, `GetX`, `GetY`, `SetFlagman`, `Save`, `GetCommands`, `SetCommands`,
  `AskPlace`, `GetVidName`, `SetPosition`, `SetVid`, `SetAutoScroll`.
- **Sessione**: tutte le altre (menu, testo, file, audio, video, input, rete come giocatore
  singolo, sistema).

## Regole per i branch dell'ondata 2

- `src/game/engine_api.hpp` è il punto d'incontro: modificarlo solo se indispensabile, in modo
  compatibile, e segnalarlo nel resoconto.
- Il motore non dipende dalla sessione; la sessione non dipende dall'implementazione del motore
  (solo dal contratto).
- Valgono le regole di `AGENTS.md` (dati del gioco mai nel repo, test con `GROMADA_DATA`, 60/120 fps).

## Zoom e risoluzione (ondata 4)

- **Zoom analogico** (non presente nell'originale, solo presentazione): `src/game/view_zoom.hpp`.
  L'input produce `input::PcState::zoomRate` (−1..+1 per frame: grilletti L2/R2 analogici nel
  twin-stick, A/B tenuti nello schema a puntamento, 0 nei menu). La sessione possiede un
  `ViewZoom`, lo integra col tempo reale del frame (0,5×–2×, esponenziale) e lo passa alla pipeline
  di rendering, che decide la vista (dimensione = uscita / zoom) e la conversione schermo ↔ mondo
  per cursore, touch e record di input.
  Per i controlli (issue #19) la pipeline deve anche: convertire allo stesso modo
  `PcState::aimTarget` (il punto mirato dello schema diretto, anche fuori schermo: è il punto del
  mouse nel record, come `moveTarget`), dare a `input::FrameInfo::zoom` lo zoom corrente (pixel
  di schermo per pixel di mondo: scala gittata e corridoi di aggancio) e posizionare giocatore e
  bersagli di `ControlsFrame` in pixel di schermo dopo lo zoom. Con lo zoom a 1 tutto coincide.
  Fatto nella #20: `aimTarget` passa per `ScreenMapping` nello stesso ramo di `moveTarget`
  (`InputRecordBuilder::build(pc, mapping, …)`), `f.zoom = s.mapping().zoom` in `runSession`,
  giocatore e bersagli convertiti con `mapping()`; `weaponRange`/`aimLift` in pixel di mondo.
- **Risoluzione interna** e **interpolazione** delle posizioni: solo nel disegno. La simulazione,
  le demo, l'oracolo e i save state non cambiano.
- Agenti: controlli (`zoomRate`, schemi), pipeline di rendering (`ViewZoom` → vista, risoluzione
  interna, interpolazione), miglioramenti grafici (passaggio separato sopra la pipeline).

### Pipeline di rendering (issue #20, dettagli in `docs/rendering.md`)

- Contratto: ganci opzionali di `Engine` con implementazione vuota (compatibili):
  `viewOrigin(ViewRequest)`, `drawView(fb, ViewRequest, ViewOrigin)`, `screenEffectView()`,
  `drawnPosition(unit, clock)`. Senza di essi la sessione usa `camera()` e `draw()` (zoom 1).
- Spazi: **logico** (640/854 × 480: cursore, touch, input mapper, menu, testi, barra di stato),
  **vista del motore** (`camera()`: uscita / zoom pixel di mondo sui livelli), **immagine**
  (k × logico, `Session::width/height`; l'input mapper usa `logicalWidth/Height`).
- Lo zoom si applica al mondo dei livelli (`maps\levels\…`), torna a 1 sotto i menu e i briefing,
  mai nelle demo; i menu non sono mai zoomati. `ControlsFrame`: giocatore e bersagli in pixel
  logici dell'uscita, convertiti con la mappa schermo ↔ mondo dell'ultima immagine; gittata in
  pixel di mondo (il mapper la scala con `FrameInfo::zoom`).
- Opzioni core: `gromada_render_scale` (1x–4x), `gromada_smooth` (movimento fluido, attivo),
  `gromada_zoom` (zoom impostato), `gromada_render_threads` (automatico).
- **Gancio per i miglioramenti grafici (#21)**: `Session::setWorldPass(fn)`; `fn(const WorldPass&)`
  riceve a ogni frame disegnato, prima di menu/testi/barra/effetti, il mondo a risoluzione interna
  (modificabile), la trasformazione (`origin`, `scale`, `screenScale`, `zoom`, `renderScale`),
  `renderClock` e la lista `DrawnSprite` (id, vid, classe, categoria, posizione disegnata
  sub-pixel/estrapolata nel mondo e nell'immagine, rettangolo del corpo) in ordine di disegno.
  Va collegato in `startSession()` di `src/gromada_libretro.cpp` (un solo gancio: se serve più di
  un passaggio, una funzione che li chiama in ordine). Vedi `docs/rendering.md` §6.

## Miglioramenti grafici (ondata 4, issue #21)

Effetti opzionali di sola presentazione, spenti di default: luci dinamiche e ombre di contatto
(progetto, costi e proposta GPU in `docs/enhancements.md`). Modulo header-only
`src/game/enhance/` (`analysis.hpp`, `enhance.hpp`): dipende solo da `core/` e `libgromada/`;
riceve gli sprite come `enhance::DrawItem`, non conosce `Sprite` né `World`.

**Agganci** (in `src/game/engine.hpp`; attivi solo se un effetto è acceso, altrimenti il disegno
e la lista di disegno sono identici bit per bit):

- `GameEngine::draw` (via originale, vista identica): `enh = activeEnhancer()`, `enh->begin()`;
  ogni sprite disegnato (catene comprese) passa da `put()` → `enh->drawn`; **dopo gli strati 0–4,
  prima dello strato 5** `enh->ground(fb, fxView, occluders)` con gli sprite di `m_sorted`;
  **dopo l'ultimo strato** `enh->finish(fb, fxView)` (luci).
- `GameEngine::drawView` (pipeline della #20): gli stessi punti, ma nella **lista di disegno**:
  `drawSpriteView` aggiunge l'oggetto a `enh->drawn` con la posizione disegnata (sub-pixel,
  estrapolata); quando la lista arriva allo strato 5 `enh->prepareGround(...)` accumula le ombre e
  la lista riceve un comando `view::BandPass` (`Enhancer::groundBand`); a fine lista
  `prepareLights(...)` e il comando `Enhancer::lightBand`. `view::drawBand` esegue i comandi
  `BandPass` al loro posto, sulle righe della banda: l'immagine è la stessa per ogni numero di
  bande/thread. Trasformazione: `enhance::View{origine, scala = zoom × k}`.
- Poi la sessione disegna menu, testi, barra di stato, `Effect()` e cursore: gli effetti non li
  toccano. Il gancio `Session::setWorldPass` (#20) non è usato dagli effetti e resta libero.

Le impostazioni (`enhance::Settings`) appartengono al chiamante:
`GameEngine::setEnhancements(const Settings*)`, lette a ogni disegno; il core le legge dalle
opzioni `gromada_fx_lights` e `gromada_fx_contact_shadows` e passa il puntatore in
`makeGameEngine()`. Nessuna modifica al contratto `game::Engine` né alla sessione.

Stato: le impostazioni e le cache (`m_fxSettings`, `m_fx`) non sono stato del gioco e non vanno
nei save state (`GameEngine` +16 byte nel controllo di `savestate_session.hpp`: 3264 → 3280).
