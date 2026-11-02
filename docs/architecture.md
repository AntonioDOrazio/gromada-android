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
