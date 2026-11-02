# Miglioramenti grafici opzionali (issue #21)

Effetti di **sola presentazione**, **disattivati di default**, sopra il disegno fedele del mondo
(`GameEngine::draw`, colore a 16 bit di `docs/engine.md` §8). Non toccano la simulazione, le demo,
l'oracolo né i save state: con gli effetti spenti l'uscita è identica bit per bit.

| opzione core | valori | effetto |
|---|---|---|
| `gromada_fx_lights` — *Luci dinamiche (spari, esplosioni)* | disattivate, tenui, normali, intense | alone colorato attorno a laser, plasma, proiettili, lanciafiamme, esplosioni, vampate delle armi, macchie di luce |
| `gromada_fx_contact_shadows` — *Ombre di contatto (occlusione ambientale)* | disattivate, tenui, normali, marcate | alone scuro sfumato sul terreno alla base di edifici, unità, alberi, rocce |

Codice: `src/game/enhance/analysis.hpp` (cosa emette luce, nuclei d'ombra), `src/game/enhance/enhance.hpp`
(opzioni, aggancio, griglie, le due passate). Aggancio nel disegno: `docs/architecture.md`
("Miglioramenti grafici"). Strumenti: `tools/engine_run --fx L,S [--fx-scale N]` (screenshot
prima/dopo, costo per frame), test `tests/enhance_test.cpp` (CI), `tests/enhance_data_test.cpp` e
`tests/enhance_e2e.sh` (con i dati).

## 1. Cosa c'è nei dati del 1999

- **Strati** (`z_layer` dei vid): 0–3 terreno e piattaforme, 4 decalcomanie (crateri, "macchie
  dopo l'esplosione"), **5** lo strato riordinato di unità, edifici, alberi, rocce *e* di
  proiettili ed esplosioni, 8 missili in volo, 10–15 immagini di schermo.
- **Luce già presente**: i formati grafici 3 (livello di schiarimento unico) e 4 (livello per
  canale, quindi colorato) sono "macchie di luce" dell'originale (×8/(8−L) sul pixel sotto):
  vampate delle armi, luce del laser, del plasma, delle esplosioni, della lava. Sono sprite a
  parte, figli degli spari e delle esplosioni.
- **Trasparenze** (formato 8, opacità op/7): galo, esplosioni, fumo, polvere, scie.
- **Ombre**: pixel d'ombra disegnati negli sprite (comando `0x40`, dimezzano il terreno) e ombra
  proiettata degli oggetti in quota (flag `0x80` e z > 0). Non c'è profondità né normali.
- **Catene**: ogni vid ha 16 figli per azione (`childNvid`); quelli di `act_fight` (8) e
  `act_death` (15) generano proiettili, raggi, vampate, esplosioni, fumi, detriti, relitti.

Inventario misurato su `fw.res` (369 vid): 172 vid candidati come sorgenti (§2.1), con 5939 frame:
**1453 frame emettono luce**, 4486 restano scuri (fumo, polvere, detriti, relitti).

## 2. Luci dinamiche (implementato, software)

### 2.1 Sorgenti: dalle classi, dalle catene e dai pixel

1. **Candidati** (`classifyEmitters`, una volta per motore): i vid delle classi Projectile,
   CannonMissile, Effect1, Effect2, Shell, AviaMissile, più ogni vid raggiungibile dai figli di
   `act_fight`/`act_death` di qualunque vid (seguendo poi tutti i figli: vampata → raggio →
   esplosione → macchia di luce → fumo). Nessun nome di vid è usato.
2. **Emissione per frame** (`frameEmission`, in cache per frame): dai pixel del frame
   effettivamente disegnato, quindi un'esplosione si accende e si spegne con la sua animazione:
   - pixel di luce (formati 3/4): il loro livello per canale (colore della luce dell'originale),
     peso 0,35 perché l'originale li ha già applicati;
   - pixel colorati (formato 8 pesato per l'opacità; formato 2 solo per proiettili piccoli,
     ≤ 16 px): "calore" = luminosità oltre il 55% × (0,3 + 0,7 × saturazione): fuoco, laser,
     plasma brillano, fumo grigio e polvere no; i grandi sprite opachi (detriti, relitti) mai.
   Ne escono potenza P (pixel emittenti equivalenti), colore normalizzato, baricentro e
   dispersione. Sotto P = 6 il frame non fa luce.
3. **Sorgenti del frame**: gli sprite davvero disegnati (catene comprese) con un frame emittente;
   la luce sta nel baricentro dell'emissione, abbassata di z/2 verso il suolo.

Raggio R = clamp(max(14 + 2,6·√P, 1,5·dispersione), 20, 150) px di mondo; intensità
I = clamp(0,2 + √P/45, 0, 1). Esempi: raggio laser (P≈40) R≈30; esplosione grande (P≈2400) R≈140.

### 2.2 Accumulo e applicazione

- Griglia a bassa risoluzione allineata al framebuffer, **4 px di mondo per cella** (la cella è
  4 × scala px del framebuffer: la griglia è 161×121 a qualunque risoluzione interna); ogni luce
  somma I·(1 − d²/R²)²·colore sulle celle del suo cerchio. Poi un ginocchio l/(1 + 0,35·l), così
  dieci esplosioni sovrapposte non bruciano tutto.
- Applicazione con **upsampling bilineare** solo sulle celle toccate e una curva di
  **esposizione** per canale: energia e = l·(mul·d + 255·add); out = d + (255 − d)·(1 − e^(−e/255))
  (tabella di 1024 voci). Il terreno scuro prende colore, i toni medi si schiariscono, i pixel già
  luminosi (il nucleo dell'esplosione) non si saturano in bianco.
- Livelli (mul, add, guadagno): tenui (1,2; 0,10; 0,7), normali (1,6; 0,14; 1,0), intense
  (1,8; 0,16; 1,2).
- Nessuna sorgente visibile → nessuna cella toccata → nessun pixel visitato.

## 3. Ombre di contatto / occlusione ambientale approssimata (implementato, software)

Gli sprite non hanno profondità: l'occlusione è stimata dalla **sagoma** dello sprite e dal suo
**ingombro** a terra.

- **Occlusori**: gli sprite visibili dello strato 5 di categoria Object o Vehicle con
  `sizeZ` ≥ 4 (edifici, unità, alberi, rocce, mura); niente aerei (hanno l'ombra proiettata);
  peso 1 − z/24 per chi è in quota (salto della Kassandra), 1 con `ZeroZ`.
- **Nucleo per frame** (`occlusionKernel`, in cache): i pixel opachi e trasparenti del frame
  (non la luce né i pixel d'ombra già disegnati) **nelle righe dell'ingombro** (`sizeY` attorno
  al centro del frame, che è la posizione dello sprite: verificato sovrapponendo i rettangoli di
  collisione agli screenshot), ridotti a celle di 2×2 px di mondo, sfocati con due box blur di
  raggio ~ `sizeZ`·0,22 (4–11 px). Più alto l'oggetto, più ampio l'alone.
- **Quando**: dopo gli strati 0–4 e **prima** dello strato 5. I nuclei sono sommati su una griglia
  (2 px di mondo per cella, spostamento bilineare per le posizioni sub-cella) e moltiplicati sul
  terreno; poi gli oggetti si disegnano sopra e coprono la loro parte: resta un alone morbido che
  segue il piede reale di cupole, carri, tronchi, e si somma dove gli oggetti sono vicini (come
  l'AO negli anfratti).
- Intensità al bordo: tenui 0,32, normali 0,46, marcate 0,62 (copertura × 1,7, al massimo 1).
- Cache: ~50 µs per nucleo, al più 24 nuovi nuclei per frame (il primo frame di un livello non si
  blocca: gli aloni compaiono nei primi 2–3 frame); tutti i nuclei del livello 19 = 7,3 MB, la
  cache si svuota oltre 32 MB.

Limiti noti: le piattaforme dello strato 3 (es. il centro servizi) sono terreno e ricevono l'alone
degli edifici vicini; gli sprite con l'ombra già disegnata la sommano all'alone (voluto, è più
scuro solo al piede).

## 4. Costi misurati

Desktop (Xeon virtuale condiviso, build Release, `engine_run --fx L,S --fx-scale 2`, miglior
tempo su 15 ripetizioni dello stesso frame; un ciclo minimo che scurisce 640×480 pixel costa qui
0,31 ms = 1,0 ns/pixel). La 2× è il mondo disegnato a 1× e ingrandito, con gli effetti calcolati
a 2× (quello che riceverà l'aggancio con la risoluzione interna di #20).

| scena | occlusori / luci | ombre 1× | luci 1× | **totale 1×** | ombre 2× | luci 2× | **totale 2×** |
|---|---|---|---|---|---|---|---|
| battaglia, livello 19 (laser, esplosioni) | 268 / 79 | 0,35 ms | 0,51 ms | **0,86 ms** | 0,89 ms | 1,42 ms | **2,31 ms** |
| stessa battaglia, 2 s dopo | 270 / 38 | 0,36 | 0,30 | **0,66** | 0,90 | 0,87 | **1,77** |
| base con edifici, livello 8 | 117 / 0 | 0,29 | 0 | **0,29** | 0,80 | 0 | **0,80** |
| foresta, livello 5 | 202 / 12 | 0,37 | 0,05 | **0,42** | 0,93 | 0,15 | **1,09** |

Su tutta la battaglia (279 frame a 1×, livelli intense/marcate): media 0,80 ms, p99 1,15 ms,
massimo 1,84 ms; il disegno del mondo nello stesso frame costa 1,5 ms. I costi non dipendono dal
livello scelto (cambiano solo le costanti). Quasi tutto è l'applicazione per pixel (ombre
~2,4 ns/px a 1× e 1,7 a 2×, luci ~4,7 e 3,9 ns/px): scala con i pixel toccati, non con il numero
di sorgenti.

**Stima sul Nova** (QCS8550, Cortex-X3 3,2 GHz sul thread di RetroArch): codice intero scalare,
prestazioni per core paragonabili a questo desktop; con margine termico ×1,0–1,5:

- 1×, 120 Hz (8,33 ms per frame): **≤ 1,3 ms** nel caso peggiore misurato (16% del frame);
  ampiamente nel budget insieme al disegno del mondo (~1,5–2,3 ms) e alla simulazione (≤ 0,5 ms).
- 2×: **≤ 3,5 ms** nel caso peggiore, da sommare al disegno del mondo a 2× di #20: a 120 Hz è
  stretto, a 60 Hz (16,7 ms) non è un problema.
- Flip 2 (SD865, Cortex-A77 2,84 GHz, 60 Hz): ×1,6–2,2 rispetto al desktop, ≤ 1,9 ms a 1×.

Memoria: griglie ~1,2 MB (indipendenti dalla risoluzione), cache dei nuclei ≤ 32 MB (tipico
2–8 MB), cache dell'emissione trascurabile.

## 5. Effetti valutati: software o GPU

| effetto | software (CPU) | GPU (Vulkan `hw_render`) | scelta |
|---|---|---|---|
| luci dinamiche colorate | sì, §2, 0,3–0,5 ms 1× / 0,9–1,4 ms 2× | gratis, anche per pixel con normali stimate | **fatto** |
| ombre di contatto / AO | sì, §3, 0,3–0,4 ms 1× / 0,8–0,9 ms 2× | gratis | **fatto** |
| bloom / bagliore | una passata a schermo intero (riduzione, sfocatura, somma): ≥ 2 passate ≈ ≥ 0,6 ms a 1×, ≥ 2,5 ms a 2× (dal ciclo minimo misurato) | gratis | non nel core: i preset slang di RetroArch (bloom/glow) lo fanno già sulla GPU senza codice nostro; le luci danno già l'alone alle sorgenti |
| lampo a schermo delle esplosioni | economico (una moltiplicazione globale) ma copre l'intera immagine e l'originale ha già `eff_nuke` per i momenti voluti dagli script | gratis | scartato (cambia il ritmo voluto dagli script) |
| ombre morbide degli oggetti in quota | richiede una passata d'ombra separata nel disegno (l'ombra proiettata oggi dimezza il terreno in un colpo) | semplice | rimandato: tocca `putcadr` |
| luce con normali (rilievo degli sprite) | troppo caro per pixel | normali dalla luminanza degli sprite, luci per pixel | fase 3 GPU |
| distorsione di calore (esplosioni, lava, lanciafiamme) | caro (rilettura sfalsata per pixel) | gratis | fase 3 GPU |
| tinta giorno/notte, nebbia | economico | gratis | non richiesto: cambia l'arte; di notte le luci dinamiche renderebbero molto |

## 6. Proposta GPU (libretro `hw_render` Vulkan), in fasi

Il core oggi produce un framebuffer XRGB8888 e RetroArch applica Vulkan e gli shader slang. Il
passaggio al rendering nativo (`RETRO_ENVIRONMENT_SET_HW_RENDER` con
`RETRO_HW_CONTEXT_VULKAN` e l'interfaccia di negoziazione Vulkan) va fatto per fasi, sempre con
il percorso software come riferimento e come ripiego (driver senza Vulkan, test, oracolo).

- **Fase 0 (ora, costo zero)**: effetti software di questo documento + preset slang di RetroArch
  per scalatura, bloom, CRT. Basta a 1× e 2×.
- **Fase 1 — composizione su GPU** (intervento contenuto): il core carica a ogni frame
  l'immagine del terreno (dopo lo strato 4) e quella finale del mondo come texture a 1×, più le
  due griglie (a 640×480: luce 161×121×RGB, ombra 321×241) come texture minuscole; uno shader fa
  l'upsampling bilineare, applica AO solo dove l'immagine finale coincide col terreno, la curva
  di esposizione delle luci e lo scaling/zoom. Il costo CPU degli effetti scende all'accumulo
  sulle griglie (< 0,1 ms) a qualunque risoluzione; la simulazione e il disegno a 16 bit restano
  identici. Rischi: la gestione del contesto Vulkan in libretro (perdita/ricreazione del contesto,
  sincronizzazione delle immagini), screenshot e save state di RetroArch (che leggono il frame).
  **Conviene solo se** la risoluzione interna 3–4× di #20 su CPU supera il budget del Nova.
- **Fase 2 — sprite su GPU** (intervento grande): atlante delle grafiche decodificate (indici +
  palette per vid, con gamma e palette alternative), disegno in lotti nell'ordine dello strato
  (algoritmo del pittore invariato) con shader che riproducono le operazioni a 16 bit
  dell'originale (dimezzamento delle ombre, schiarimento ×8/(8−L), trasparenza op/7, ombra
  proiettata): sono già isolate in `hicolor.hpp` e verificate dall'oracolo, quindi portabili in
  GLSL con test di confronto per pixel (readback). Dà risoluzione interna e zoom senza costo CPU.
  Rischi: fedeltà bit per bit (i test di hash dovranno leggere la GPU), driver Vulkan Android,
  complessità del mantenimento di due renderer.
- **Fase 3 — effetti solo GPU**: luci per pixel con normali stimate dagli sprite, ombre morbide
  direzionali, distorsione di calore, particelle, bloom HDR delle sorgenti.

Raccomandazione: restare in fase 0; misurare #20 sul Nova a 2×–4× e passare alla fase 1 solo se il
disegno a risoluzione interna non sta nel budget. La fase 2 ha senso solo se si vogliono 4× e zoom
fluido a 120 Hz.

## 7. Dati e verifiche

- Dati usati: classi e categorie dei vid, `childNvid` di `act_fight`/`act_death`, `sizeX/Y/Z`,
  `z_layer` (lo strato 5), flag `ZeroZ`, formato e pixel dei frame (palette originale, non quella
  con la gamma: le luci non seguono `SetGamma`).
- `tests/enhance_test.cpp` (CI, frame sintetici nei formati 2/4/8): opzioni, catene,
  emissione (fuoco sì, fumo no, luce blu del formato 4, relitti mai), nuclei, passate (spento =
  immagine invariata; ombre solo più scure al piede; luci solo più chiare attorno alla sorgente;
  nessuna sorgente = nessun pixel visitato; la stessa immagine a 2×; bordi e 16:9).
- `tests/enhance_data_test.cpp` (con `GROMADA_DATA`): battaglia del livello 19 e base del
  livello 8, motori con lo stesso input: impostazioni assenti / presenti ma spente / accese e poi
  spente → **hash dei frame identici** a quelli del motore senza effetti; con tutto acceso lo
  **stato del motore** (byte del save state) è identico; la catena del laser della Kassandra è tra
  le sorgenti.
- `tests/enhance_e2e.sh` (con i dati, 60 e 120 fps): il core tramite `tools/harness` riproduce la
  demo 1 per 1500 frame; opzioni predefinite e opzioni esplicitamente "disattivate" danno gli
  stessi hash di video e audio frame per frame; con luci e ombre accese il video cambia
  (1496 frame su 1500) e l'audio resta identico in tutti i frame (stessa simulazione).
- Screenshot prima/dopo verificati (a 1× e 2×): battaglia con laser ed esplosioni (livello 19),
  base con edifici (livello 8), foresta (livello 5), le armi della Kassandra (lanciafiamme,
  cannone, missili, mitragliatrice) sul livello 1:

      engine_run --level 19 --spawn 4520,1720 --input "300:fire:300,-100:8000" \
                 --shots 2500,4500 --fx 2,2 --fx-scale 2
      engine_run --level 8 --spawn 1400,1450 --shots 1000 --fx 2,2 --fx-scale 2
