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
| Telecamera | Automatica (segue il giocatore nello schema diretto e col touch) / Segue sempre il giocatore / Originale (scorrimento ai bordi) | Automatica |
| Mira assistita (schema diretto) | Attiva / Disattivata | Attiva |

## Mappatura (Nova e Flip 2)

I due dispositivi hanno gli stessi comandi (d-pad, ABXY, due stick, L1/R1, L2/R2, L3/R3,
Start/Select, touchscreen), quindi la mappatura è la stessa; cambiano schermo e frequenza
(vedi più sotto).

| Comando | Cursore virtuale (predefinito) | Diretto |
|---|---|---|
| Stick sinistro | Muove il cursore (curva + accelerazione) | Guida: la Kassandra va nella direzione dello stick (ordini di movimento), lasciato si ferma |
| Stick destro | Cursore lento e preciso (30 %, lineare) | Mira (aggancia il bersaglio più vicino alla direzione); oltre il 70 % spara |
| D-pad | Frecce (comandi "da carro" dell'originale) | uguale |
| A (destra) | Click sinistro: muovi / conferma | Click sinistro sul punto mirato (vai lì) |
| B (basso) | Click destro: attacca | Click destro sul punto mirato (spara) |
| L1 / R1 | Arma precedente / successiva tra quelle disponibili (tasti `1`-`4`) | uguale |
| X (alto) | `Invio` | uguale |
| Start | `Esc` (menu) | uguale |
| Select | `TAB` (schermo informazioni) | uguale |
| Tocco breve | Click sinistro nel punto | uguale |
| Trascinamento | Tasto sinistro tenuto, il cursore segue il dito | uguale |
| Pressione lunga (0,45 s) | Click destro tenuto (attacco continuo) finché il dito resta giù | uguale |
| Due dita | Click destro nel punto del primo dito | uguale |
| Mouse (USB/BT) | Cursore relativo, tasti sinistro/destro, rotella = armi | uguale |

Nei **menu** (contesto `Menu`, deciso dalla sessione: menu, schermate di briefing, filmati) lo
schema diretto torna al cursore virtuale: i menu del gioco si usano col puntatore.

**Inserimento del nome** (contesto `TextEntry`, senza tastiera): d-pad su/giù sceglie la
lettera (con ripetizione automatica dopo 0,4 s, 12 al secondo), A o d-pad destra la scrive,
B o d-pad sinistra = `Backspace`, Start = `Invio`, Select = `Esc`. Il motore mostra la lettera
in selezione (`PcState::textCandidate`). Con una tastiera fisica si scrive direttamente.
Solo col touch vale ciò che vale col mouse nell'originale: un tocco conferma il nome (anche vuoto),
una pressione lunga (click destro) annulla.

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
| Touchscreen | In 4:3 i tocchi sulle bande nere laterali sono ignorati | RetroArch riporta "premuto" solo dentro l'immagine; un dito sulle bande non annulla gli altri |
| Vista | In 16:9 si vede più campo ai lati; i menu 640×480 sono centrati (i tocchi vanno sui pulsanti come si vedono) | Il guinzaglio della telecamera tiene la Kassandra ad almeno 54 px dal bordo, come in 4:3 |

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
  pulsanti. Il ciclo salta le armi non disponibili e genera la pressione del tasto `1`-`4`
  corrispondente, come farebbe il giocatore su PC. Disponibilità e arma scelta vengono dallo
  script del gioco, con la stessa regola di `Tact()` in `HEADER.INI`: lo slot *k* ha una testa
  (`weapon[k]`) e, tranne il primo, munizioni (`act_getammo`); l'arma in uso è `nweapon`.
  L'originale **rifiuta** il cambio mentre la torretta sta caricando o scaricando: il tasto viene
  ripetuto ogni 0,12 s finché il gioco non conferma il cambio (al massimo 1,5 s), e due pressioni
  rapide avanzano di due armi a partire da quella chiesta, non da quella ancora montata.
- **Arma a schermo.** La barra di stato dell'originale (`TAB`) mostra l'arma; quando è nascosta,
  dopo ogni cambio (o pressione di L1/R1) compare per 2 s in basso a sinistra
  `numero NOME: TIPO  munizioni` (con `...` finché il cambio non è avvenuto), su un riquadro scuro
  perché il carattere del gioco è verde come gran parte del terreno.
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
- **Guida dello schema diretto.** Le frecce dell'originale sono comandi "da carro armato"
  (su = avanti, giù = indietro a metà velocità, sinistra/destra = sterza solo in movimento):
  legarle allo stick in 8 settori farebbe girare il carro invece di portarlo dove punta lo stick.
  Lo stick sinistro dà quindi **ordini di movimento** che l'originale capisce: un click sinistro
  120 px davanti alla Kassandra nella direzione dello stick (mai a meno di 48 px: l'originale
  considera arrivato un ordine entro 24×16 px), rinnovato ogni 0,2 s e subito se lo stick gira di
  oltre 8°; lasciato lo stick, un ordine a 3 px dai suoi piedi la ferma (frena con la sua inerzia,
  circa mezzo secondo). L'ordine non sposta il cursore disegnato: solo il record di input di quel
  frame porta il click nel punto dell'ordine, senza tasto destro (`Player::Tact` sparerebbe lì).
  Un click vero nello stesso frame ha la precedenza e l'ordine slitta al frame dopo. I tempi sono
  in secondi: a 60 e 120 fps gli ordini cadono negli stessi istanti (entro un frame). Il d-pad
  resta sulle frecce, per chi vuole i comandi originali.
- **Mira dello schema diretto.** Il cursore è il punto mirato, sulla direzione dello stick
  destro alla gittata dell'arma in uso (dal motore, nella metrica del gioco `max(|dx|, 1,5·|dy|)`;
  se esce dallo schermo è accorciato lungo la stessa retta, così la torretta punta esattamente
  lì). La **mira assistita** aggancia il bersaglio (unità ostile colpibile dall'arma, sullo
  schermo, entro la gittata) più vicino alla direzione, entro 26° per lato: il cursore va sul
  bersaglio (forma FIGHT) e il click destro dell'originale attacca quell'unità. Senza aggancio i
  colpi cadono nel punto mirato, come col mouse. Fino al 70 % della corsa si mira soltanto, oltre
  si spara. Lasciato lo stick la direzione resta e B spara lì; con lo stick destro fermo e il
  sinistro in uso la mira segue la direzione di marcia (la torretta guarda avanti e aggancia ciò
  che sta davanti). Toccare lo schermo o muovere il mouse sposta il punto mirato dove si indica.
- **Telecamera.** Nello schema diretto e col touch il cursore non può scorrere la vista ai bordi,
  e l'originale sposterebbe la vista solo col "guinzaglio" (Kassandra a 54 px dal bordo). Con
  *Telecamera = Automatica* la vista **segue il giocatore**: nulla si muove finché resta nel 10 %
  centrale, poi la vista lo raggiunge in modo esponenziale (indipendente dal frame rate) e si
  sposta in avanti nella direzione di marcia (a 100 px/s il giocatore sta circa 60 px dietro il
  centro: si vede cosa c'è davanti). Si ferma in pausa, nei menu, con `ScrollLock(1)` degli
  script e nelle demo; i limiti della mappa e il guinzaglio restano quelli del motore. Con *Segue
  sempre* vale anche per il cursore virtuale: portare il cursore al bordo scorre la vista come
  nell'originale e il seguire riprende quando la Kassandra si muove. *Originale* lo disattiva.
- **Bordi e touch.** L'originale scorre la vista quando il mouse è entro 5 px dal bordo. Il
  cursore virtuale lo fa (spingendolo contro il bordo), ma un tocco vicino al bordo o il punto
  mirato no: per questi il record di input tiene la posizione "schermo" a 6 px dal bordo (il
  punto nel mondo, dove si clicca, resta esatto). Senza questo, dopo un tocco sul bordo il cursore
  fermo lì farebbe scorrere la vista all'infinito.
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

## Per il motore e la sessione (API)

```cpp
gromada::input::Mapper mapper;                 // configure(readSettings(getVar)) dalle opzioni
const game::ControlsFrame c = session.controlsFrame(); // giocatore, bersagli, armi (dal motore e dallo script)
FrameInfo f{.dt = dt, .width = w, .height = h, .context = session.inputContext(),
            .player = &playerPosition, .weaponMask = c.weaponMask, .currentWeapon = c.currentWeapon,
            .aimDistance = c.weaponRange, .targets = c.targets};
const PcState& pc = mapper.update(pollRaw(input_state_cb, mapper.settings()), f);
// pc.cursor, pc.left/right (down/pressed/released), pc.key(Key::Num1..), pc.text, pc.move,
// pc.moveOrder/moveTarget (ordine di movimento), pc.edgeScroll, pc.aimLocked, pc.weaponPending
session.setCameraFollow(...);                  // secondo l'opzione Telecamera, lo schema e il touch
session.runFrame(pc, dt);                      // InputRecordBuilder: PcState -> record originale
```

Il motore espone `Engine::playerAim()` (aggiunta compatibile al contratto, implementazione vuota
di default): posizione del flagman, gittata dell'arma in uso e unità ostili che l'arma può
colpire, con il punto che le mette "sotto il cursore" (centro del riquadro di `unitAt`).
Senza questa query la sessione ricava la posizione del giocatore da `Flagman`/`GetX`/`GetY`.

## Prove automatiche

- `input_mapper_test`: schemi, ordini di movimento (anche lo stesso ritmo a 60 e 120 fps), mira
  assistita, richieste d'arma ripetute, touch, mouse, tastiera, nome.
- `controls_test`: record di input con ordini e fascia dei bordi, telecamera che segue (zona
  morta, anticipo, 60 = 120 fps, 16:9, pausa con lo scorrimento ai bordi), armi dallo script,
  indicatore dell'arma.
- `controls_e2e_{pad,touch}_{60,120}fps_{43,169}` (con `GROMADA_DATA`, `tests/controls_e2e.sh`):
  attraverso il core e l'harness sul livello 1.
  *pad*: menu solo col pad, poi schema diretto: 1 s a destra e 2,5 s in su, la Kassandra si ferma
  dove si lascia lo stick (entro ~7 px tra 60 e 120 fps), la vista l'ha seguita, lo stick destro
  aggancia la torretta davanti e la colpisce (i punti vita dei bersagli scendono).
  *touch*: menu solo col touch (tocchi per filmati e titolo, pressione lunga = nuovo giocatore,
  tocco = conferma il nome, tocco su NORMALE, AVVIO), poi un tocco 180 px a destra porta lì la
  Kassandra e la vista la segue.
  I controlli usano la traccia del core: `GROMADA_TRACE=<n>` nell'ambiente scrive ogni *n* frame
  `trace map=… ctx=… flagman=x,y cam=x,y cursor=x,y weapon=… mask=… pending=… targets=… hp=…
  order=… locked=… nearest=dx,dy`.

## Prove con l'harness

    tools/harness build/gromada_libretro.so <gioco>/gromada.exe 200 /tmp/shot gromada_level=5 \
        "lstick=0.6,0.3@0-40" "btn=a@70-90" "btn=b@110-130" "touch=500,400@140-180" shots=80,120,175

Input a tempo (ripetibili): `btn=<nome>@da-a`, `lstick=x,y@…`, `rstick=x,y@…`,
`touch=px,py[;px,py]@…` (dita in pixel dell'immagine), `mouse=dx,dy@…`, `mouse_btn=left|right@…`.
