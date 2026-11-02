# Pipeline di rendering: zoom, risoluzione interna, movimento fluido (issue #20)

Presentazione moderna del gioco **senza toccare la simulazione**: demo, oracolo (`trace_oracle`) e
determinismo dei save state restano quelli di prima. Con risoluzione interna 1×, zoom 1 e
movimento fluido disattivato l'immagine è **bit per bit** quella del renderer originale (stesso
codice: `Engine::draw()`), verificato frame per frame su tutto il percorso del gioco (§7).

## 1. File

| file | contenuto |
|---|---|
| `src/game/view_raster.hpp` | `view::Raster` (trasformazione di vista in virgola fissa), `drawFrameScaled()`: `putcadr` (`hicolor::FrameVisitor`) per qualunque scala e posizione sub-pixel |
| `src/game/render_view.hpp` | `HiResComposite` (immagine a risoluzione interna con menu/testi/effetti), `drawCursorScaled()` |
| `src/game/engine.hpp` | `GameEngine::viewOrigin()`, `drawView()`, `motion()`, `drawnPosition()` |
| `src/game/engine_api.hpp` | contratto (ganci opzionali): `ViewRequest`, `ViewOrigin`, `DrawnSprite`, `ScreenEffectView` |
| `src/game/session.hpp` | zoom (`ViewZoom`), vista del motore, mappa schermo ↔ mondo, `drawWorld()`, `present()`, gancio `WorldPass` |
| `src/game/session_input.hpp` | `ScreenMapping`, `InputRecordBuilder::build(pc, mapping, ...)` |
| `src/gromada_libretro.cpp` | opzioni core, `retro_get_system_av_info`, `SET_SYSTEM_AV_INFO`, traccia `GROMADA_TRACE_VIEW` |
| `tools/render_bench.cpp` | tempi per frame (CPU del thread) per livello, scala, zoom, formato; screenshot |
| `tests/render_view_test.cpp` | test sintetici (CI) |
| `tests/render_data_test.cpp`, `tests/render_e2e.sh` | test sui dati del gioco (locali) |

## 2. Opzioni core

| opzione | valori | effetto |
|---|---|---|
| `gromada_render_scale` | 1x (predefinito), 2x, 3x, 4x | l'immagine è k × 640×480 (k × 854×480 in 16:9) |
| `gromada_smooth` | attivato (predefinito), disattivato | movimento fluido (§4) |
| `gromada_zoom` | 1x, 0.5x, 0.75x, 1.5x, 2x | zoom impostato quando l'opzione cambia (poi i grilletti / A-B, issue #19) |

`retro_get_system_av_info`: `base` = dimensione logica × k, `max` = 854k × 480k (copre i due
formati: il cambio di formato resta un `SET_GEOMETRY`). Quando cambiano k, il frame rate o la
modalità visualizzatore il core chiama `SET_SYSTEM_AV_INFO`. Il visualizzatore resta a 1×.

## 3. Zoom e vista

Spazi:

- **logico**: 640×480 o 854×480 (`Session::logicalWidth/Height`). Cursore, touch, input mapper,
  menu, testi, barra di stato: tutto come prima.
- **vista del motore** (`Engine::camera()`): sui livelli `round(larghezza / zoom) ×
  round(480 / zoom)` pixel di mondo. Bordo di scorrimento (5 px), guinzaglio, clamp alla mappa,
  `SetPosition` e la camera che segue il giocatore lavorano su questa vista, con le regole
  dell'originale. Il guinzaglio usa ora `larghezza − 54 × altezza − 40` per ogni vista (era
  `max(586, …)`: identico per le viste ≥ 640×480, D8 di `docs/engine.md`).
- **immagine**: k × logico. Un pixel di mondo vale `zoom × k` pixel dell'immagine.

Zoom (`ViewZoom`, contratto della #19): la sessione integra `PcState::zoomRate` col tempo reale del
frame. Lo zoom **applicato** (`effectiveZoom()`) segue quello del giocatore con una transizione
esponenziale (0,12 s) e torna a 1 mentre c'è un menu o un briefing sopra il livello (i briefing e
il menu di gioco sono disegnati sulla mappa del livello). Fuori dai livelli (`maps\levels\…`), nelle
demo e con motori senza `drawView()` lo zoom applicato è 1. Quando la vista cambia dimensione
resta centrata dov'era, su un'**ancora sub-pixel** (uno zoom continuo non deriva). I menu non sono
mai zoomati: sono disegnati sull'immagine logica (§5).

Origine della vista (`Engine::viewOrigin`): la sessione propone il centro (posizione sub-pixel
della camera che segue il giocatore, oppure l'ancora dello zoom, oppure la camera); il motore
riapplica il guinzaglio al flagman **dove è disegnato** (movimento fluido: la vista lo segue senza
gli scatti dei suoi tick) e, sui livelli (`fitMap`), tiene la vista nella mappa; se la mappa è più
piccola della vista la centra.

Schermo ↔ mondo (`ScreenMapping`, salvato nello stato): il punto dell'uscita `p` è il punto di mondo
`origine + (p − viewX) / zoom` dell'**ultima immagine mostrata** (il giocatore punta ciò che vede).
Il record di input (`OriginalInput`) porta quel punto come posizione nel mondo e, come posizione
sullo schermo, il cursore nella vista del motore (`(p − viewX) / zoom`): con lo zoom il bordo
dell'uscita resta il bordo per lo scorrimento. Con zoom 1, risoluzione 1× e movimento fluido
spento la mappa è `mondo = schermo + camera` dell'originale (stesso record bit per bit, test). I
menu ricevono il cursore logico. Il punto del mouse nel record è, nell'ordine, il bersaglio di un
ordine di movimento (`moveTarget`), il punto mirato dello schema diretto (`aimTarget`, issue #19,
anche fuori schermo) o il cursore: tutti passano per la stessa mappa. Per l'input mapper
(`controlsFrame()`) giocatore e bersagli sono in pixel logici dell'uscita dopo lo zoom (stessa
mappa); gittata (`weaponRange`) e sollevamento antiaereo (`aimLift`) restano in pixel di mondo e
`input::FrameInfo::zoom` (impostato in `runSession`) dà al mapper lo zoom dell'ultima immagine,
con cui scala gittata e corridoi di aggancio (issue #19).

## 4. Movimento fluido

L'originale aggiorna ogni sprite al suo tick (`frameDuration`: 71 ms di solito, 39 la Kassandra,
28 i missili): a 60/120 fps le posizioni restano ferme per più frame e poi saltano. Il disegno
(`drawView` con `extrapolate`) mette ogni sprite **dove è all'istante dell'immagine**
(`renderClock` = orologio del gioco + la frazione di ms non ancora data al motore):

- posizione sub-pixel: `x + fracX/128` (gli accumulatori a 7 bit di `fx::moveStep`; lo stesso per
  `y` e, con velocità verticale, per `z`);
- più lo spostamento che farà il tick successivo, calcolato come la fisica (`SpriteTact`): velocità
  integrata (`fx::integrateSpeed`, accelerazione e frenata), direzione, vento, velocità verticale
  con gravità, per il tempo trascorso dall'ultimo tick (`lastTact`), al più un periodo del tick
  (+1/8 di jitter dei vid `Randomized` + 20 ms). Quando il tempo arriva al dt del tick, la
  posizione disegnata è quella che il tick produrrà: nessuno scatto.

È una funzione **pura dello stato** della simulazione e dell'istante: niente storia per sprite,
quindi nessun problema con save state, rewind e run-ahead. Di conseguenza:

- teletrasporti (`act_changecoor`, script), creazioni, morti: si disegna lo stato corrente, mai
  un'interpolazione tra due posizioni;
- raggi (`speed` istantanea), sprite passivi, `SkipUpdate`, pausa: nessuna estrapolazione;
- sotto-oggetti (torrette, effetti agganciati) si muovono con la radice della catena;
- fotogrammi d'animazione, direzioni discrete, ordine di disegno (strato 5 ordinato per y, z, x)
  e culling usano lo stato della simulazione: identici all'originale;
- limite di sicurezza: uno spostamento estrapolato oltre 48 px non viene applicato.

Camera: la camera che segue il giocatore (issue #16) riceve, con il movimento fluido, la
posizione disegnata del giocatore (`Engine::drawnPosition`) e la sua posizione sub-pixel diventa
il centro della vista; il guinzaglio è riapplicato alla posizione disegnata (§3). Lo scorrimento ai
bordi dell'originale (passi interi `dt·v >> 10` per frame) resta com'è.

Limite noto: un nuovo ordine di movimento (`act_move`) azzera gli accumulatori sub-pixel nella
simulazione; la posizione disegnata perde quella frazione (≤ 1 px di mondo). Nello schema diretto
succede a ogni ordine dello stick (circa ogni 200 ms).

## 5. Risoluzione interna

`k = 1`: il mondo si disegna nell'immagine logica (`drawView` a scala `zoom`, o `draw()` a zoom 1
senza movimento fluido). `k > 1` (`HiResComposite`):

1. `drawView` disegna il mondo a k × logico (scala `zoom × k`, posizioni sub-pixel; gli sprite
   restano bitmap scalate nearest-neighbour; gli sprite di schermo, `st_screen`, a scala k);
2. il gancio `WorldPass` (§6);
3. l'immagine logica = il mondo campionato al centro di ogni blocco k × k; menu, testi, barra di
   stato, radar, indicatore dell'arma ed `Effect()` disegnano lì **con il codice di sempre**;
4. composizione: i pixel logici cambiati dall'UI diventano blocchi k × k (scala nitida); le
   operazioni di colore di `Effect()` (lampi, dissolvenze) sono applicate a ogni pixel del mondo ad
   alta risoluzione; fuori dalla finestra di `eff_openwindow` si vede lo schermo precedente; tutto
   in un solo passaggio con la conversione in XRGB8888;
5. il cursore è scalato di k (nearest).

Il rasterizzatore (`drawFrameScaled`) copre con ogni pixel sorgente i pixel dell'immagine tra le
proiezioni dei suoi due bordi, calcolate in virgola fissa (1/256 di pixel di mondo, scala 16.16)
dalle coordinate di mondo: due sprite che si toccano nel mondo si toccano sullo schermo a ogni
scala (niente fessure tra le tessere del terreno). A scala intera ogni colonna sorgente copre
esattamente k colonne anche in posizione sub-pixel (percorso veloce). Le operazioni sui pixel
(palette, ombre, luce dei formati 3/4, trasparenza del formato 8, ombra proiettata) sono quelle di
`hicolor::FrameVisitor`, applicate a ogni pixel coperto.

Culling: ±512 px attorno al centro per una vista 640×480 come l'originale, allargato per le viste
più grandi (`512 + max(0, metà − 320)` in x, `− 240` in y). Nel percorso originale (1×, zoom 1,
senza movimento fluido) resta ±512 anche in 16:9.

Save state: l'immagine salvata durante le demo è quella logica (un pixel per blocco k × k), quindi
la dimensione dello stato non dipende da k; al caricamento viene ingrandita.

## 6. Gancio per i miglioramenti grafici (issue #21)

`Session::setWorldPass(std::function<void(const WorldPass&)>)`: chiamato a ogni frame disegnato,
subito dopo il mondo e **prima** di menu, testi, barra di stato ed effetti:

```cpp
struct WorldPass {
    FramebufferRef world;            // il mondo a risoluzione interna (k × vista), modificabile
    ViewOrigin origin;               // punto di mondo nel pixel (0, 0)
    double scale;                    // pixel dell'immagine per pixel di mondo (zoom × k)
    double screenScale;              // pixel dell'immagine per pixel logico (k)
    double zoom; int renderScale;
    double renderClock;              // orologio del gioco (ms) dell'immagine
    bool extrapolated;               // posizioni estrapolate (movimento fluido)
    std::span<const DrawnSprite> sprites;   // in ordine di disegno
};
struct DrawnSprite {
    UnitId id; int nvid; std::uint8_t cls, category;   // classe (cls::) e categoria (cat::) del vid
    bool screen, child;              // sprite di schermo; sotto-oggetto della radice precedente
    float worldX, worldY, worldZ;    // posizione disegnata (sub-pixel, estrapolata)
    float fbX, fbY;                  // punto a terra (worldX, worldY) nell'immagine
    int left, top, right, bottom;    // rettangolo del corpo nell'immagine
};
```

Pixel dell'immagine = `(punto di mondo − origin) × scale`. Con il gancio impostato la sessione usa
sempre il percorso nuovo (anche a 1×). Un passaggio di luci dei proiettili può filtrare
`cls == cls::Projectile/CannonMissile/AviaMissile` e usare `fbX, fbY − worldZ × scale`. Lo stato del
gancio non è salvato.

## 7. Verifica

- **Uscita invariata (hash)**: `tests/render_e2e.sh` (con `GROMADA_DATA`) gioca tutto il percorso
  (logo, filmati, menu, profilo, mappa, briefing, livello 1 con lo schema a cursore e con lo schema
  diretto) a 4:3/60 fps e 16:9/120 fps con movimento fluido spento e confronta l'hash di ogni
  frame (immagine e audio) con i valori registrati dal core di `main` prima di questa issue
  (144d5d3, dopo la #19). `render_view_test`: `drawView` con la vista identità è `draw()` bit per bit.
- **Rasterizzatore** (`render_view_test`, CI): i 40 casi sintetici di `putcadr` dell'oracolo
  (formati 2/3/4/8, ombra proiettata) identici a scala 1 e, a scala 2 e 3, identici
  all'ingrandimento k × k; nessuna fessura a scale 0,5…8 e origini sub-pixel.
- **Matematica della vista** (`render_view_test`): mappa schermo ↔ mondo, record identico
  all'originale senza zoom (cursore, ordini di movimento, cursore fuori dal bordo), zoom 2 e 0,5,
  origine (clamp, mappe piccole centrate, guinzaglio sulla posizione disegnata).
- **Interpolazione** (`render_view_test`): posizione continua tra due tick (scarto massimo dal moto
  uniforme 0,0066 px a 120 fps; senza, l'unità sta ferma in 160 frame su 180), teletrasporto non
  interpolato, raggi e pausa non estrapolati, un proiettile nuovo parte già in moto.
- **Combinazioni** (`render_data_test`, con `GROMADA_DATA`): risoluzione 1–4, zoom 0,5/1/2, 4:3 e
  16:9, 60 e 120 fps sul livello 1: nessun crash, dimensione dell'immagine, zoom applicato, il
  giocatore disegnato dove la mappa schermo ↔ mondo dice e il cursore messo lì punta nel record il
  suo punto di mondo.
- Demo, oracolo e save state: `trace_oracle`, `savestate_*` (con il movimento fluido attivo, il
  valore predefinito) restano verdi.

### Fluidità (120 fps, livello 1, `GROMADA_TRACE_VIEW`)

Passi per frame della posizione disegnata mentre l'oggetto si muove (\|Δpasso\| = media del
cambiamento del passo tra frame consecutivi, 0 per un moto uniforme; "fermo" = frame senza
movimento dentro un tratto di moto). Prima = movimento fluido spento (l'originale), dopo = acceso.

| oggetto (schema) | prima: \|Δpasso\|, fermo, sd | dopo: \|Δpasso\|, fermo, sd |
|---|---|---|
| veicolo nemico, mondo | 1,34 px, 89 %, 1,95 | 0,009 px, 0 %, 0,155 |
| Kassandra, mondo (diretto) | 1,15 px, 81 %, 1,23 | 0,048 px, 1,8 %, 0,215 |
| Kassandra, mondo (cursore) | 0,80 px, 83 %, 0,98 | 0,080 px, 28 %*, 0,27 |
| scorrimento della vista (diretto, camera che segue) | 0,58 px, 32 %, 0,57 | 0,019 px, 6,5 %, 0,31 |
| scorrimento della vista (cursore, bordi) | 0,56 px, 68 %, 1,58 | 0,54 px, 64 %, 1,57 (invariato, §4) |
| proiettile (y − z) | 3,26 px, 82 %, 3,38 | 0,044 px, 21 %**, 0,89 |

\* la Kassandra striscia contro un ostacolo: si ferma davvero nei tick rifiutati (prima della
verifica dell'ostacolo vibrava: 0,27 px, massimo 4 px). \*\* comprende l'esplosione ferma. Lo
scarto tipo residuo del veicolo è la sua accelerazione (il passo cresce davvero); i passi anomali
residui della Kassandra nello schema diretto sono gli ordini di movimento (§4).

## 8. Prestazioni

`tools/render_bench --level 19` (15.200 sprite, il livello più pesante), 120 fps, movimento
fluido attivo, tempo CPU di un thread su un core Xeon Granite Rapids (VM a 2 vCPU condivisa);
"immagine" = frame con disegno − frame senza (la simulazione costa 0,17 ms).

| immagine (ms) | 4:3 | 16:9 |
|---|---|---|
| originale 1× (`draw()`, senza movimento fluido) | 1,7–1,8 | 2,1 |
| 1× | 1,9–2,1 | 2,5–2,7 (culling più largo in 16:9) |
| 2× | 5,7 | 6,9 |
| 3× | 9,0 | 10,9 |
| 4× | 14–15 | 16,4 |
| zoom 0,5 a 1× / 2× | 3,3 / 5,2 | 4,7 / 7,2 |
| zoom 2 a 1× / 2× | 2,2 / 6,4 | 2,5 / 7,4 |

Con i thread (`gromada_render_threads`, automatico = core − 1, al più 4; la lista di disegno è
costruita su un thread, poi le bande dell'immagine e la composizione sono in parallelo; immagine
identica per ogni numero di thread) — tempo reale, 4:3, 1 → 2 thread sulla VM: 1× 2,3 → 2,0 ms,
2× 5,9 → 3,9 ms, 3× 9,2 → 6,3 ms, 4× 15,5 → 9,5 ms.

**Stima per il Retroid Pocket Nova** (QCS8550: 1 Cortex-X3 a 3,2 GHz, 2 A715 + 2 A710 a 2,8 GHz,
3 A510): un X3 è circa 1,4–1,7 volte più lento di questo core su questo carico (intero e memoria),
un A715/A710 circa 2–2,5 volte; con 4 thread la parte parallela (~85 % da 2× in su) va su ~3
"core X3 equivalenti". Livello 19, budget a 120 Hz 8,3 ms (meno simulazione ~0,3 ms e la copia
dell'immagine nel frontend):

| Nova, livello 19 | 1 thread | 4 thread (automatico) | 120 Hz |
|---|---|---|---|
| 1× (640×480) | ~3,5 ms | ~1,5–2 ms | sì |
| 2× (1280×960, nativo) | ~9 ms | ~4–4,5 ms | **sì** (consigliato) |
| 3× (1920×1440) | ~14 ms | ~6–6,5 ms + copia di 11 MB | al limite: non garantito sui livelli pesanti |
| 4× (2560×1920) | ~23 ms | ~9–10 ms + copia di 20 MB | **no** (va a 60 Hz) |

Sul Flip 2 (16:9, 1080p a 60 Hz, budget 16,7 ms) 2× (1708×960) e 3× stanno nel budget; oltre
1080p 4× non serve. Sono stime: da verificare sul dispositivo con `render_bench` (Termux) o con
le statistiche di RetroArch.
