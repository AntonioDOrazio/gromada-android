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
- **Risoluzione interna** e **interpolazione** delle posizioni: solo nel disegno. La simulazione,
  le demo, l'oracolo e i save state non cambiano.
- Agenti: controlli (`zoomRate`, schemi), pipeline di rendering (`ViewZoom` → vista, risoluzione
  interna, interpolazione), miglioramenti grafici (passaggio separato sopra la pipeline).

## Miglioramenti grafici (ondata 4, issue #21)

Effetti opzionali di sola presentazione, spenti di default: luci dinamiche e ombre di contatto
(progetto, costi e proposta GPU in `docs/enhancements.md`). Modulo header-only
`src/game/enhance/` (`analysis.hpp`, `enhance.hpp`): dipende solo da `core/` e `libgromada/`;
riceve gli sprite come `enhance::DrawItem`, non conosce `Sprite` né `World`.

**Aggancio** (in `GameEngine::draw`, `src/game/engine.hpp`; attivo solo se un effetto è acceso,
altrimenti `draw()` è identico bit per bit):

1. `enh = activeEnhancer()` all'inizio (nullptr se le impostazioni mancano o sono tutte spente),
   `enh->begin()`;
2. ogni sprite disegnato, catene comprese, passa da `put()`: `drawSprite` più
   `enh->drawn.push_back(fxItem(s))`;
3. **dopo gli strati 0–4, prima dello strato 5** (gli sprite visibili e ordinati di `m_sorted`):
   `enh->ground(fb, fxView, occluders)` scurisce il terreno al piede degli oggetti, che poi gli si
   disegnano sopra;
4. **dopo l'ultimo strato**: `enh->finish(fb, fxView)` (luci). Poi la sessione disegna menu,
   testi, barra di stato, `Effect()` e cursore: gli effetti non li toccano.

Dati dell'aggancio: il framebuffer del mondo (qualunque risoluzione), `enhance::View`
{`originX`, `originY`, `scale`} con pixel = (mondo − origine) × scala (oggi la camera e 1), e per
ogni sprite un `DrawItem` {frame disegnato, x, y, z di mondo, nvid, classe, categoria,
`sizeX/Y/Z`, `ZeroZ`}. Le impostazioni (`enhance::Settings`) appartengono al chiamante:
`GameEngine::setEnhancements(const Settings*)`, lette a ogni `draw()`; il core le legge dalle
opzioni `gromada_fx_lights` e `gromada_fx_contact_shadows` e passa il puntatore in
`makeGameEngine()`. Nessuna modifica al contratto `game::Engine` né alla sessione.

**Al merge con la pipeline di rendering (#20)**: `fxView` diventa la camera interpolata e la
scala (risoluzione interna × zoom); `fxItem()` deve usare le stesse posizioni (interpolate) di
`drawSprite`; se il mondo si disegna su un framebuffer a risoluzione interna, `ground()` e
`finish()` vanno chiamati su quel framebuffer (prima dello strato 5 e dopo l'ultimo), prima della
riduzione all'uscita e della UI. Le griglie degli effetti hanno celle in pixel di mondo × scala,
quindi la loro dimensione non cresce con la risoluzione; il costo per pixel sì (misure in
`docs/enhancements.md` §4).

Stato: le impostazioni e le cache (`m_fxSettings`, `m_fx`) non sono stato del gioco e non vanno
nei save state (`GameEngine` 3184 → 3200 byte nel controllo di `savestate_session.hpp`).
