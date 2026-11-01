# Comandi: schemi di controllo, cursore, touchscreen

Gromada è un gioco per mouse: tasto sinistro = movimento, destro = attacco, `1`-`4` = armi,
frecce = movimento da tastiera, `TAB` = schermo informazioni, `Esc` = menu principale.
Il modulo `src/input/` traduce i comandi della console portatile in questo stato "da PC"
(cursore, due tasti del mouse, tasti, testo digitato), che è l'unica cosa che il motore legge.

I nomi dei pulsanti sono quelli del RetroPad di RetroArch, per **posizione**:
**A** = destra, **B** = basso, **X** = alto, **Y** = sinistra. Se sul dispositivo le etichette
stampate sono diverse (o A/B sono scambiati nelle impostazioni Android), conta la posizione.
Tutto è rimappabile da RetroArch (*Comandi → Comandi porta 1*): le descrizioni dei pulsanti
dicono l'azione di gioco e, tra parentesi, il tasto PC equivalente.

## Opzioni core

| Opzione | Valori | Predefinito |
|---|---|---|
| Schema di controllo | Cursore virtuale / Diretto | Cursore virtuale |
| Velocità cursore (pixel/s) | 300–1000 | 500 |
| Accelerazione cursore (stick in fondo) | 1x (nessuna), 1.5x, 2x, 3x, 4x | 2x |
| Precisione stick | Normale (curva 2) / Alta (curva 3) / Lineare | Normale |
| Touchscreen e mouse | Entrambi / Solo touchscreen / Solo mouse / Disattivati | Entrambi |
| Tastiera fisica | Disattivata / Attiva | Disattivata |

## Mappatura (Nova e Flip 2)

I due dispositivi hanno gli stessi comandi (d-pad, ABXY, due stick, L1/R1, L2/R2, L3/R3,
Start/Select, touchscreen), quindi la mappatura è la stessa; cambiano schermo e frequenza
(vedi più sotto).

| Comando | Cursore virtuale (predefinito) | Diretto |
|---|---|---|
| Stick sinistro | Muove il cursore (curva + accelerazione) | Movimento: frecce, 8 direzioni |
| Stick destro | Cursore lento e preciso (30 %, lineare) | Mira; spinto oltre il 70 % spara (click destro sul punto mirato) |
| D-pad | Frecce | Frecce |
| A (destra) | Click sinistro: muovi / conferma | Click sinistro sul cursore |
| B (basso) | Click destro: attacca | Click destro sul punto mirato |
| L1 / R1 | Arma precedente / successiva (tasti `1`-`4`) | uguale |
| X (alto) | `Invio` | uguale |
| Start | `Esc` (menu) | uguale |
| Select | `TAB` (schermo informazioni) | uguale |
| Tocco breve | Click sinistro nel punto | uguale |
| Trascinamento | Tasto sinistro tenuto, il cursore segue il dito | uguale |
| Pressione lunga (0,45 s) | Click destro tenuto (attacco continuo) finché il dito resta giù | uguale |
| Due dita | Click destro nel punto del primo dito | uguale |
| Mouse (USB/BT) | Cursore relativo, tasti sinistro/destro, rotella = armi | uguale |

Nei **menu** (contesto `Menu`, deciso dal motore) lo schema diretto torna al cursore
virtuale: i menu del gioco si usano col puntatore.

**Inserimento del nome** (contesto `TextEntry`, senza tastiera): d-pad su/giù sceglie la
lettera (con ripetizione automatica dopo 0,4 s, 12 al secondo), A o d-pad destra la scrive,
B o d-pad sinistra = `Backspace`, Start = `Invio`, Select = `Esc`. Il motore mostra la lettera
in selezione (`PcState::textCandidate`). Con una tastiera fisica si scrive direttamente.

### Retroid Pocket Nova — 4:3, 1280×960, 120 Hz

| Impostazione | Consiglio | Perché |
|---|---|---|
| Formato | 4:3 (640×480) | Riempie lo schermo, scala intera 2× |
| Frame rate | 120 | Cursore e scorrimento più fluidi; la velocità non cambia (vedi sotto) |
| Cursore | 32×32 px di gioco = 64×64 px fisici | Le dimensioni originali restano leggibili |
| Touchscreen | Coordinate su tutta l'immagine | Nessuna banda nera: ogni punto dello schermo è un punto di gioco |

### Retroid Pocket Flip 2 — 16:9, 1920×1080, 60 Hz

| Impostazione | Consiglio | Perché |
|---|---|---|
| Formato | 16:9 (854×480) | Riempie lo schermo e mostra più campo ai lati |
| Frame rate | 60 | La frequenza del pannello |
| Cursore | 32×32 px di gioco ≈ 72×72 px fisici | |
| Touchscreen | In 4:3 i tocchi sulle bande nere laterali sono ignorati | RetroArch riporta "premuto" solo dentro l'immagine |

Su entrambi: in RetroArch lasciare *Tipo da analogico a digitale* = **Nessuno** per la
porta 1 (altrimenti lo stick sinistro preme anche il d-pad) e nascondere l'overlay a schermo
(*Nascondi overlay se è collegato un controller*), che altrimenti intercetta i tocchi.
Con un mouse collegato scegliere *Touchscreen e mouse* = **Solo mouse**: RetroArch può
riportare il mouse anche come puntatore, e con i gesti touch tenere fermo il tasto sinistro
per 0,45 s diventerebbe un click destro.

## Motivazioni

- **Due schemi.** Il cursore virtuale riproduce fedelmente il gioco (si clicca dove andare
  e cosa colpire) e serve comunque per i menu; lo schema diretto è il controllo "twin-stick"
  naturale su una console: si guida con uno stick e si mira con l'altro, senza inseguire il
  carro armato col cursore.
- **A = sinistro, B = destro.** Come chiede la issue; A è il "conferma" di RetroArch e il click
  sinistro serve anche nei menu. Chi preferisce l'attacco sul pulsante in basso può scambiarli
  dal remap di RetroArch.
- **Armi su L1/R1.** Le armi sono quattro: scorrerle con i dorsali lascia i pollici su stick e
  pulsanti. Il ciclo salta le armi non disponibili (`FrameInfo::weaponMask`), e genera la
  pressione del tasto `1`-`4` corrispondente, come farebbe il giocatore su PC.
- **Start = Esc, Select = TAB.** Convenzione delle console: pausa/menu e mappa/informazioni.
  I pulsanti sono "tenuti" come il tasto vero (pressione e rilascio arrivano al motore).
- **D-pad = frecce in entrambi gli schemi.** Il gioco originale supporta il movimento da
  tastiera; nel cursore virtuale lo stick sinistro è già occupato dal cursore.
- **Curva e accelerazione.** La risposta è `deflessione^2`: a metà corsa il cursore va al 25 %
  della velocità, per puntare al pixel; tenendo lo stick in fondo per 0,25 s la velocità sale
  in 0,6 s fino al moltiplicatore scelto, per attraversare lo schermo in fretta. Girando lo
  stick di oltre 45° l'accelerazione riparte da 1x (niente sorpassi quando si torna indietro).
  Lo stick destro, lento e lineare, serve per le correzioni fini.
- **Indipendenza dal frame rate.** Le velocità sono in pixel al secondo e l'accelerazione è
  integrata in forma chiusa su ogni frame: a 60 e a 120 fps lo stesso input sposta il cursore
  dello stesso tratto (verificato nei test e con l'harness: stick inclinato per 0,5 s, immagine
  a 1 s identica al pixel a 60 e 120 fps). Anche ripetizione dei tasti e pressione lunga sono a
  tempo, non a frame.
- **Mira dello schema diretto.** Il punto mirato è a 160 px dal giocatore (un terzo dell'altezza
  dello schermo; il motore può passare la gittata dell'arma in `FrameInfo::aimDistance`).
  Fino al 70 % della corsa si mira soltanto (il cursore FIGHT mostra il bersaglio), oltre si
  spara: così si può prendere la mira senza sprecare munizioni. Lasciato lo stick, il punto
  mirato resta e segue il giocatore, e B spara lì. La posizione del giocatore arriva dal motore
  tramite l'interfaccia `PlayerPosition`; senza giocatore (il viewer) si usa il centro schermo.
- **Touch.** Il click sinistro parte al rilascio del dito, non al contatto: così una pressione
  lunga può diventare attacco senza aver prima dato un ordine di movimento. Trascinando oltre
  10 px il tasto sinistro resta premuto e il cursore segue il dito. Ogni click dura almeno un
  frame, anche se il tocco è più breve. RetroArch su Android emula il mouse dai tocchi
  ("quick tap"): per non avere doppi click il mouse è ignorato durante i tocchi e per 0,35 s dopo.
- **Tastiera disattivata di serie.** Su desktop RetroArch associa i tasti della tastiera anche al
  RetroPad (es. Invio = Start), e leggerli entrambi darebbe doppi comandi. Si attiva per scrivere
  il nome con una tastiera vera.

## Viewer (fino all'arrivo del gameplay)

Il viewer non ha ancora un giocatore: la telecamera ne fa le veci.

| Comando | Azione nel viewer |
|---|---|
| Frecce (d-pad; stick sinistro nello schema diretto) | Scorrono la telecamera |
| Cursore sul bordo dello schermo (16 px) | Scorrimento stile RTS, più veloce più si è vicini al bordo (non col touch: toccare vicino al bordo non deve far scorrere) |
| Y tenuto | Scorrimento veloce |
| L2 / R2 | Livello precedente / successivo |
| L3 / R3 | Effetto sonoro precedente / successivo |
| L1 / R1 | Cambio arma (messaggio a schermo) |

Questi comandi stanno su pulsanti che il gioco non usa, quindi non si sovrappongono alla
mappatura definitiva. Lo scorrimento ai bordi parte quando il cursore entra nella fascia,
quindi tra 60 e 120 fps può differire al massimo di un frame di scorrimento (≈ 5 px).

Forma del cursore nel viewer (nel gioco la sceglierà il motore in base a cosa c'è sotto):
CYCLE per 1 s dopo il caricamento di un livello, FIGHT col click destro o mirando, MOVE col
click sinistro, NOMOVE se lo scorrimento ai bordi è bloccato dal bordo della mappa, ARROW
altrimenti.

## Cursori originali

I cinque cursori (`cursores\arrow.cur`, `move`, `nomove`, `fight`, `cycle`, come li nomina
`gromada.exe`) sono file `.CUR` di Windows 32×32 a 1 bit con maschere AND/XOR e hotspot.
Si leggono dalla cartella del gioco senza distinguere maiuscole e minuscole (sul CD sono
`ARROW.CUR` ecc., e il file system di Android distingue). Il parser accetta anche 4/8/24/32 bit.
Se mancano, si usa una freccia disegnata nel codice. I file del gioco non vanno nel repository.

## Per il motore (API)

```cpp
gromada::input::Mapper mapper;                 // configure(readSettings(getVar)) dalle opzioni
FrameInfo f{.dt = dt, .width = w, .height = h, .context = Context::Gameplay,
            .player = &playerPosition, .weaponMask = 0b1111};
const PcState& pc = mapper.update(pollRaw(input_state_cb, mapper.settings()), f);
// pc.cursor, pc.left/right (down/pressed/released), pc.key(Key::Num1..), pc.text, pc.move
mapper.setWeaponSlot(n);                       // se l'arma cambia per altri motivi
drawCursor(cursors.get(CursorShape::Fight), pc.cursorX(), pc.cursorY(), fb, w, h, pitch);
```

## Prove con l'harness

    tools/harness build/gromada_libretro.so <gioco>/gromada.exe 200 /tmp/shot gromada_level=5 \
        "lstick=0.6,0.3@0-40" "btn=a@70-90" "btn=b@110-130" "touch=500,400@140-180" shots=80,120,175

Input a tempo (ripetibili): `btn=<nome>@da-a`, `lstick=x,y@…`, `rstick=x,y@…`,
`touch=px,py[;px,py]@…` (dita in pixel dell'immagine), `mouse=dx,dy@…`, `mouse_btn=left|right@…`.
