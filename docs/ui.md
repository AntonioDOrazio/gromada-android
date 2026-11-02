# UI: testo bitmap, testi a schermo, menu

Modulo `src/ui/` (header-only, namespace `gromada::ui`), issue #5. Riproduce come l'originale
disegna testi e schermate di menu, pronto per le builtin degli script (`PrintText`, `PrintFile`,
`DeleteText`, `LoadMenu`, `FreeMenu`, `MenuCreate`, `MenuAction`). La logica degli script non è
qui: le schermate si costruiscono chiamando le stesse primitive che userebbero gli script.

Fonti: dati della versione italiana (`fw.res`, `maps/`), script `maps/*.INI` e disassemblato di
`gromada.exe` (PE32 del 6/12/1999). Gli indirizzi citati sono VA dell'exe.

## API in breve

| File | Contenuto |
|---|---|
| `ui/text_layout.hpp` | `layoutText`, `measureText`, `wrapText`, `wrapTextToWidth`, `FontMetrics`, `Align` (logica pura, senza dati) |
| `ui/font.hpp` | `FontSet`: metriche, `hasGlyph`, `drawGlyph`, `drawText` sui vid di `fw.res` |
| `ui/text_manager.hpp` | `TextManager`: `print`/`printFile`/`erase` con handle, `setVisibleCount` (macchina da scrivere), `update(dt)`, `takeSounds()`, `draw` |
| `ui/menu.hpp` | `parseMenuFile`/`loadMenuFile` (.men), `MenuScreen` (`load`, `create`, `setAction`, `destroy`, `clear`, `draw`), costanti `vids::*` |
| `ui/lens.hpp` | `applyLensFrame`: formato grafico 7 (lente del monitor) |
| `ui/game_texts.hpp` | `resolveGamePath` (percorsi degli script, case-insensitive), `readGameText`, `parseDefines`, `Locale`, percorsi di briefing/riassunti/armi |
| `ui/text_encoding.hpp` | `gameToUtf8`, `utf8ToGame` |
| `ui/ui.hpp` | include tutto; `drawUi(menu, texts, fonts, fb, ox, oy)` disegna menu e testi nell'ordine dell'exe |

Corrispondenza con le builtin (numeri da `maps/HEADER.INI`):

| Builtin | Chiamata |
|---|---|
| `PrintText(x,y,text,font)` 116 | `TextManager::print(x, y, bytes, font)` → handle > 0 (0 se il testo è vuoto) |
| `PrintFile(x,y,file,font)` 117 | `resolveGamePath` + `TextManager::printFile` → handle, 0 se il file non si apre |
| `DeleteText(t)` 118 | `TextManager::erase(t)` (0 e handle sconosciuti ignorati) |
| `Action(t, act_settextcount, n, 0)` | `TextManager::setVisibleCount(t, n)` |
| `LoadMenu(file)` 71 | `MenuScreen::load(loadMenuFile(...))` (aggiunge, non svuota) |
| `FreeMenu()` 72 | `MenuScreen::clear()` |
| `MenuCreate(nvid,dir,x,y)` 102 | `MenuScreen::create(nvid, dir, x, y)` → numero unico |
| `MenuAction(nvid,dir,action)` 101 | `MenuScreen::setAction(nvid, dir, action)` (`act_destroy` = 54 rimuove) |
| `GetVidName(nvid)` 124 | `Vid::name` (byte CP866, già localizzati in `fw.res`) |

Anteprima desktop (fuori dal core):

    cmake -S . -B build -G Ninja -DGROMADA_BUILD_TESTS=ON && cmake --build build --target ui_preview
    build/ui_preview /percorso/gioco /tmp/anteprime level=1 [res=800]
    ffmpeg -i /tmp/anteprime/briefing01.ppm briefing01.png

Scrive menu principale, crediti, briefing (sopra il livello, con lente), briefing a metà
battitura, scelta missione, giocatori, opzioni, centro servizi e la tavola del font.
**Le immagini contengono grafica e testi protetti: non vanno nel repository.**

## Font

`fw.res` (edizione italiana) contiene **un solo** vid di classe `Font` (19): nvid **127**
"Font", cioè `V_FONT` degli script.

- Grafica: formato 2 (RLE indicizzato con palette propria), 256 frame da 7×8 pixel,
  `directionsCount` 255, `frameDuration` 71 ms, `z_layer` 14, flag `IgnoreGlobalGamma`.
- **Glifo del byte c = frame c** (non la mappatura direzione→frame degli sprite). La tavola è
  quella del font VGA in ROM: 0x01–0x1F simboli, 0x20–0x7E ASCII (minuscole comprese),
  **0x80–0x9F maiuscole cirilliche CP866** (А…Я), 0xA0–0xFF un misto di simboli CP437/box
  drawing (le minuscole cirilliche CP866 non ci sono). Il frame 0 e lo spazio sono vuoti.
- Cella: `sizeX` × `sizeY` del vid = **7 × 10**: avanzamento orizzontale 7, interlinea 10.

### Disegno (gromada.exe 0x40b624, ramo `type == 0x13`)

Un testo è un oggetto sprite in coordinate schermo (flag 4 in `[obj+0x1c]`) il cui vid è il
font; campi: `[obj+0x14]` nvid, `[obj+0x3f]` puntatore al testo, `[obj+0x43]` byte visibili.

1. Penna iniziale: `x0 = x − width/2 + sizeX/2`, `y0 = y − height/2 + sizeY/2` (shift aritmetici
   sui campi del vid). Per V_FONT: **(x, y + 1)**. Conferma dagli script: in `PLAYER.INI` il
   cursore (vid 130, 7×8, centrato) segue il nome stampato a `centr_x−59` in
   `centr_x−56+7*strlen(nome)`.
2. Per ogni byte fino a `[obj+0x43]`:
   - `'\n'`: `y += sizeY` del font corrente, `x` torna a inizio riga;
   - `'\r'`: `x` torna a inizio riga;
   - ESC (0x1B) seguito da un byte: il byte diventa l'nvid del font (la coppia occupa una cella);
     un ESC finale è ignorato;
   - altri byte < 0x20: niente;
   - byte ≥ 0x20: frame `c` del font corrente con l'angolo in alto a sinistra sulla penna;
   - in ogni caso poi `x += sizeX` del font corrente (anche per i caratteri di controllo).
3. "Inizio riga" vale `x0 − sizeX(font iniziale)`, a cui si somma l'avanzamento del byte: con
   font di larghezze diverse il ritorno a capo dipende da entrambi (riprodotto).

Non esistono a capo automatico né allineamento: i file di testo sono impaginati a mano (righe
fino a 69 colonne, centrature con spazi). `wrapText` e `Align` sono estensioni per i testi del
core.

Stranezza dell'exe non riprodotta: ESC scrive il nuovo nvid **nell'oggetto** (`[obj+0x14]`),
quindi dal frame successivo il testo ripartirebbe col font cambiato. Qui ogni disegno riparte
dal font di creazione. Nessun file del gioco contiene ESC (verificato), quindi non ha effetti.

### Macchina da scrivere (`act_settextcount`)

`PrintText`/`PrintFile` mostrano subito tutto il testo (`[obj+0x43]` = lunghezza). Lo script del
livello, all'apertura del briefing, chiama `Action(gamemenu, act_settextcount, 0, 0)`
(`HEADER.INI`, `act_settextcount` = 32, "numero di caratteri mostrati"): il testo riparte da 0 e
viene battuto. Gestore `act_nextcommand` della classe STEXT (0x40e01c):

- se il conto alla rovescia `[obj+0x33]` (millisecondi, decrementato col tempo trascorso,
  0x409b39) non è zero, non fa niente;
- altrimenti mostra un byte in più; se è uno spazio (`isspace`) mostra anche tutta la sequenza
  di spazi che segue e il primo carattere dopo; se nella sequenza c'era un `'\n'` torna indietro
  di quel carattere, imposta la pausa a **150 ms** e passa all'azione 2, altrimenti all'azione 1;
- le azioni del font suonano `nsfx[1]` = **23** (tasto) e `nsfx[2]` = **24** (a capo).

**Ipotesi** (da verificare con #3): un passo per tick d'animazione dell'oggetto, cioè ogni
`frameDuration` = 71 ms (~14 caratteri/s), e un suono a ogni passo. `TextManager::update`
usa questi valori (`TypewriterTiming`, configurabile).

## Codifiche

Tutto è in byte, con la tabella **CP866** (il gioco è russo):

- testi italiani (`W*.TXT`, `levels/NN.TXT`, `NNS.TXT`, `LOCALE.INI`, nomi dei vid): **solo
  ASCII**, maiuscole, accenti scritti come apostrofo (`UNITA'`); fine riga CRLF;
- residui russi: `maps/levels/09S.TXT` contiene "АРЕНА" in CP866 (0x80 0x90 0x85 0x8D 0x80),
  che il font disegna; i commenti degli script sono CP866;
- `PrintFile` legge il file in binario (`fopen(..., "rb")`), quindi CR e LF arrivano entrambi
  al disegno; `PrintText` copia una stringa C (si ferma al primo NUL).

`gameToUtf8` converte per log e strumenti; `utf8ToGame` converte le stringhe del core: CP866
dove possibile, lettere accentate secondo la convenzione dell'edizione (`à` → `a'`, `È` → `E'`,
`ç` → `c`), il resto `?`. Il test con dati verifica che ogni byte di ogni testo del gioco e di
ogni nome di vid abbia un glifo.

## Testi localizzati

| File | Uso (script) | Posizione |
|---|---|---|
| `maps/LOCALE.INI` | `#define STR_NONAME "SENZANOME"`, `STR_NONE`, `STR_ON`, `STR_OFF`, `STR_HIGH`, `STR_LOW` (incluso da `HEADER.INI`) | – |
| `maps/levels/NN.TXT` (01–30) | briefing della missione, `GameMenuOn()` | `PrintFile(centr_x−230, centr_y−140)` |
| `maps/levels/NNS.TXT` | riassunto della mappa selezionata, `MAPS.INI` | `PrintFile(centr_x−80, centr_y+117)` |
| `maps/net/NNS.TXT` | idem per la rete, `NET.INI` | idem |
| `maps/W01.TXT`–`W12.TXT` | descrizione dell'arma, `DesignMenuTact()` | `PrintFile(centr_x−80, centr_y+117)` |
| `maps/W13.TXT`–`W15.TXT` | descrizione della trasmissione (V_TRANSMISSION1–3) | idem |
| nomi dei vid | `GetVidName` (armi, trasmissioni nel centro servizi) | – |

Nota: i `W*.TXT` **non** sono briefing, e `maps/credits.ini` è uno script: i crediti sono
un'immagine (vid 137). Gli script scrivono i percorsi con `\\` e in minuscolo
(`maps\\levels\\01.txt`) mentre i file sono `01.TXT`: `resolveGamePath` cerca ogni componente
senza distinguere maiuscole (serve su Android/Linux).

## File `.men`

14 file in `maps/`: `design`, `gamemenu` (+`gamemen8`), `maps`, `net`, `options`, `player`,
`statebar` (+`stateba8`) e le varianti `*8.men` per 800×600.

    u32 header                       = 0 in tutti i file (ignorato)
    16 × lista:
        { u16 nvid, s16 x, s16 y, s16 z, u8 direction, u8 action, payload } ...
        u16 0xFFFF

- Il record è quello della sezione "Objects" delle mappe in versione 0: il payload dipende
  dalla classe del vid (1 byte hp per gli oggetti "materiali", hp + behave per gli asset,
  niente per immagini e font). Nei file del gioco ci sono solo immagini (classe 10).
- **Lista k = layer di disegno k**: in tutti i file ogni oggetto sta nella lista uguale al
  `z_layer` del suo vid (verificato dal test). Si disegna per layer crescente.
- `x, y` sono il **centro** dell'oggetto in pixel schermo (sprite disegnato a
  `x − width/2, y − height/2`); i file 640×480 sono centrati su (320, 240), gli `*8.men` su
  (400, 300). `direction` è un byte 0–255 come nelle mappe (es. i quattro `V_4BUTTON` 0x00,
  0x40, 0x80, 0xC0); `action` è sempre 0.
- Il parser è rigoroso: errore su troncamento, nvid fuori range, classe sconosciuta, byte in
  eccesso. Tutti i 14 file vengono letti per intero.

Esempio, `gamemenu.men`: layer 10 vid 134 (lente), layer 12 vid 84 (console col monitor
trasparente) e quattro vid 222 (luci dei pulsanti). `maps.men` ha al layer 11 il vid 85
(schermo opaco con il planisfero) al posto della lente.

### Oggetti di menu e disegno

- `MenuCreate(nvid, dir, x, y)`: `dir` è un **indice di direzione** (0 … directionsCount−1),
  non un byte: lo confermano `MenuDir()` usato come indice (`nmenu=MenuDir()+26`) e
  `MenuCreate(V_TEXTMENU, 27, …)` con 34 direzioni. Il layer è lo `z_layer` del vid.
- Frame mostrato: primo frame dell'animazione (azione, direzione), con ripiego su
  `act_stand` se l'azione non ha frame (azioni dei menu: `act_menustanddown` 0 … `act_menudisable`
  6). Per ora i menu sono statici (nessuna animazione).
- Le voci dei menu non sono testo: sono immagini prerenderizzate. `V_TEXTMENU` (vid 26,
  640×480, 34 direzioni) ha un frame per stato del menu (2 giocatori, 12 opzioni, 20 centro
  servizi, 27/31 menu di gioco…); i campi da compilare (nomi, valori) sono `PrintText`.
- I testi stanno nel layer del loro font (14), sopra le voci (13) e la console (12).
  `drawUi` disegna i layer 0–15 e dopo ogni layer i testi del font di quel layer.

Vid usati dai menu (costanti in `ui::vids`; quelle con un `#define V_*` in `HEADER.INI` sono
verificate dal test con i dati, le altre vengono dai commenti degli script e dai nomi dei vid):
26 V_TEXTMENU, 222 V_4BUTTON, 292 V_RECTANGLE (zona cliccabile invisibile), 136 V_LOGO (menu
principale), 137 V_CREDITS, 85 V_SCREEN, 84 console, 134 lente, 83 cornice 800×600,
13/331 V_CHOICEMAP/V_WINNEDMAP, 298 pulsante chiuso, 296 schermo armi, 268 volume,
130 cursore del font, 86/87 V_YOURLOSE/V_YOURWIN.

## Mappe dei menu

`mainmenu.map`, `logo.map`, `credits.map`, `maps.map`, `net.map`, `player.map`, `win.map` sono
mappe normali (si leggono con `loadMap`) con **un solo oggetto**: il vid 82 ("Single ground
unit", terreno) in (671, 479), fuori schermo. La schermata la costruisce lo script con lo stesso
nome (`maps/<nome>.ini`), per esempio:

- `mainmenu.ini`: `MenuCreate(136,0,320,240)` (menu principale = immagine 136, formato 6);
  invio/spazio/clic → `player.map`, Esc/clic destro → `credits.map`, dopo 30 s una demo;
- `credits.ini`: `MenuCreate(137,0,centr_x,centr_y)` e, a 800×600, la cornice 83;
- `maps.map` → `MAPS.INI` `MapsCreate()`: `maps.men`, segnalini delle missioni, `V_TEXTMENU` 27,
  riassunto `NNS.TXT`;
- briefing: non è una mappa ma `GameMenuOn()` (`HEADER.INI`) sopra il livello in pausa:
  `V_TEXTMENU` 27 o 31, eventuale vid 298, `PrintFile(levels/NN.TXT)`, `gamemenu.men`.

`tools/ui_preview.cpp` riproduce queste ricette citando lo script di ognuna.

## Formato grafico 7: la lente del monitor

Usato solo dal vid 134 "Linza for interface monitor" (640×480, 1 frame, 58 999 byte). Non ha
colori: ridisegna ciò che è già sullo schermo comprimendolo nell'ovale del monitor (effetto
fish-eye sul livello in pausa). Decodificato da 0x404330 (il salto è nella tabella dei formati
a 0x404668/0x404694):

    u16 firstRow, u16 rowCount                    (righe 58..348 per il vid 134)
    rowCount × { s16 centerX, s16 n, u8 step[n], u8 rowStep }

Per la riga di uscita `r = top + firstRow + i` l'exe costruisce una riga temporanea di 2n−1
pixel simmetrica: con `off_0 = 0`, `off_{k+1} = off_k + step[k]`,
`tmp[n−1±k] = src[srcRow][srcCX ± off_k]`, e la copia alle colonne
`left + centerX − n … left + centerX + n − 2` (il pixel centrale cade su `centerX − 1`).
`srcCX = left + width/2`; `srcRow` parte da `top + firstRow` e cresce di `rowStep` dopo ogni riga.
L'ultimo `step` serve solo ad andare oltre la riga (mai campionato: valori casuali nei dati).
Per il vid 134 la somma dei passi è ~318 per riga e quella dei `rowStep` 423: tutta la larghezza
dello schermo e le righe 58–481 finiscono nell'ovale. L'exe lavora in place sullo stesso
framebuffer, il che funziona perché `srcRow ≥ r` per tutte le righe.

Se lo sprite non sta tutto sotto la prima riga (`top + firstRow + height − 1` oltre il fondo
del clip) l'exe sposta la sorgente a `fondo − height − 1` o, se negativa, a `firstRow` relativo
(probabile bug, irrilevante con lo sprite a schermo intero): riprodotto. Il test sintetico
verifica la mappatura, quello con dati che il frame venga consumato esattamente.

## Ipotesi e domande aperte (per #3 e il runtime)

1. ~~Cadenza della macchina da scrivere~~ (risolto, issue #13): `TextManager::tick(clock)` riproduce
   lo sprite STEXT dell'originale (`0x40e01c` nel tick dello sprite, timer di 150 ms dopo un a capo,
   suono del passo al tick successivo, posizionale): verificato sulle tre demo. `update(dt)` resta
   per gli strumenti senza orologio di gioco.
2. **SetGamma**: `GameMenuOn()` chiama `SetGamma(gammamenu)` con `gammamenu = 96 = 0x060`, il
   default è `gamma = 0x777`, il centro servizi usa `624 = 0x270`. Ipotesi: tre guadagni a 3 bit
   0xRGB (7 = neutro) applicati ai vid senza `IgnoreGlobalGamma` (flag 0x800, presente su tutti
   i vid dei menu e sul font), cioè al livello: nel briefing il livello diventa verde dentro il
   monitor. Lo strumento di anteprima lo applica in questo modo; non fa parte di `src/ui`.
3. **Ordine nello stesso layer**: qui oggetti di menu in ordine di inserimento, poi i testi in
   ordine di creazione. Nell'exe testi e oggetti di menu potrebbero stare in un'unica lista.
4. **Animazioni dei menu** (luci dei pulsanti, segnalini delle missioni, rumore dello schermo
   armi): non gestite, si disegna il primo frame dell'azione corrente.
5. **Clip della lente** con lo sprite fuori schermo: replicato il codice ma non verificato con
   casi reali (in gioco la lente è sempre a schermo intero).
6. **Header dei .men**: sempre 0; non so se l'exe lo legga.
7. **ESC nei testi**: semantica dall'exe, nessun dato la usa; l'effetto collaterale sull'nvid
   dell'oggetto non è riprodotto.
8. **16:9**: le coordinate dei menu sono quelle di 640×480 o 800×600; `draw` accetta un'origine
   per centrare la schermata in 854×480.
