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
| Mira assistita: aggancio del bersaglio (schema diretto) | Attiva / Disattivata | Attiva |
| Mirino (schema diretto) | Visibile / Nascosto (stile Geometry Wars) | Visibile |
| Larghezza corridoio di aggancio | Normale / Stretta (0.6x) / Larga (1.5x) / Molto larga (2x) | Normale |

## Mappatura (Nova e Flip 2)

I due dispositivi hanno gli stessi comandi (d-pad, ABXY, due stick, L1/R1, L2/R2, L3/R3,
Start/Select, touchscreen), quindi la mappatura è la stessa; cambiano schermo e frequenza
(vedi più sotto).

| Comando | Cursore virtuale (predefinito) | Diretto |
|---|---|---|
| Stick sinistro | Muove il cursore (curva + accelerazione) | Guida: la Kassandra va nella direzione dello stick (ordini di movimento), lasciato si ferma |
| Stick destro | Cursore lento e preciso (30 %, lineare) | Mira nella direzione dello stick con l'aggancio invisibile (vedi sotto); oltre il 70 % spara |
| D-pad | Frecce (comandi "da carro" dell'originale) | uguale |
| L2 (grilletto sinistro) | Click sinistro nel punto del cursore: muovi / conferma | Zoom indietro, analogico (più si preme, più è veloce) |
| R2 (grilletto destro) | Click destro nel punto del cursore: attacca (tenuto = fuoco continuo) | Zoom avanti, analogico |
| A (destra) | In gioco: zoom avanti finché è tenuto. Nei menu: click sinistro (conferma) | Click sinistro sul punto mirato (vai lì) |
| B (basso) | In gioco: zoom indietro finché è tenuto. Nei menu: click destro (indietro / annulla) | Click destro sul punto mirato (spara) |
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
Lo **zoom** (A/B nel cursore virtuale, L2/R2 nel diretto) vale solo **in gioco**, cioè in un livello
col giocatore (contesto `Gameplay` e una Kassandra in campo): nei menu, nelle schermate di passaggio
senza giocatore (logo, mappe tra un menu e l'altro) e nell'inserimento del nome A e B fanno ciò che
facevano (click / indietro, lettera / cancella) e `zoomRate` è 0. Un A o B ancora tenuto quando
si entra nel livello non zooma finché non viene premuto di nuovo. Lo zoom è solo presentazione: il
mapper produce la velocità (`PcState::zoomRate`, −1..+1), la pipeline di rendering lo applica
(`src/game/view_zoom.hpp`).

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
| Schema | Diretto con *Mirino = Nascosto* per giocare "alla Geometry Wars"; Cursore virtuale per giocare come su PC | L'aggancio invisibile copre tutta la gittata, anche oltre il bordo alto e basso (la gittata verticale, fino a 280 px, supera la metà dei 480 px dello schermo) |
| L2/R2 | Zoom (diretto) o click (cursore) | Grilletti analogici: la pressione decide la velocità dello zoom; se RetroArch li riporta digitali lo zoom va a velocità piena |
| Frame rate | 120 | Cursore e scorrimento più fluidi; la velocità non cambia (vedi sotto) |
| Cursore | 32×32 px di gioco = 64×64 px fisici | Le dimensioni originali restano leggibili |
| Touchscreen | Coordinate su tutta l'immagine | Nessuna banda nera: ogni punto dello schermo è un punto di gioco |

### Retroid Pocket Flip 2 — 16:9, 1920×1080, 60 Hz

| Impostazione | Consiglio | Perché |
|---|---|---|
| Formato | 16:9 (854×480) | Riempie lo schermo e mostra più campo ai lati |
| Schema | Come sul Nova | Ai lati il campo visibile (±427 px) supera la gittata: gli agganci fuori schermo sono quelli in alto e in basso |
| L2/R2 | Zoom (diretto) o click (cursore) | Come sul Nova; con grilletti digitali lo zoom va a velocità piena |
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
- **Grilletti per muovere e sparare, A/B per lo zoom (issue #19).** Nel cursore virtuale, in gioco,
  L2 = click sinistro (muovi) e R2 = click destro (attacca, tenuto = fuoco continuo) lasciano A/B
  liberi per lo zoom, come su una console. Nei menu A resta il "conferma" di RetroArch (click
  sinistro) e B il click destro (indietro/annulla dove lo era); i grilletti cliccano anche lì. Nello
  schema diretto A/B restano i click sul punto mirato e i grilletti fanno lo zoom analogico.
  Tutto è rimappabile dal remap di RetroArch.
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
- **Mira dello schema diretto (issue #19).** Lo stick destro dà la direzione; il punto mirato
  (`PcState::aimTarget`) è il punto del mouse nel record di input, anche **fuori schermo**, e il
  mirino (il cursore) è disegnato lì o, se il punto è fuori, sul bordo lungo la stessa retta; con
  *Mirino = Nascosto* non si disegna nulla finché mira il pad (touch e mouse mostrano il cursore).
  Fino al 70 % della corsa si mira soltanto, oltre si spara. Lasciato lo stick la direzione resta
  e B spara lì; con lo stick destro fermo e il sinistro in uso la mira segue la direzione di marcia
  (la torretta guarda avanti e aggancia ciò che sta davanti). Toccare lo schermo o muovere il mouse
  sposta il punto mirato dove si indica.
- **Aggancio invisibile.** Tra i bersagli che l'arma in uso può colpire (unità ostili delle
  categorie del suo `targetCategory`: gli antiaerei vedono anche gli aerei), anche fuori schermo,
  si prende il **più vicino** al giocatore dentro un **corridoio** attorno alla retta dello stick
  ed entro la **gittata**. Il punto mirato diventa il punto che mette quell'unità sotto il cursore:
  il click destro dell'originale (`Player::Tact` → `act_coor_attack`) la attacca esattamente come
  se il giocatore avesse cliccato su di lei col mouse, quindi **stesso danno**, i proiettili a
  parabola cadono su di lei e i missili a ricerca la inseguono. Senza bersaglio nel corridoio il
  colpo va **a gittata massima** lungo lo stick, per tutte le armi (anche a parabola).
  La gittata è quella dell'originale per il colpo del giocatore: `Aim` (0x40ada0) confronta la
  gittata con la distanza di `GetDirection`, che è **euclidea**, e riporta a quella distanza un
  punto più lontano. La vecchia mira (#16) metteva il punto alla gittata nella metrica del gioco
  `max(|dx|, 1,5·|dy|)` e lo teneva sullo schermo: in verticale si fermava a 2/3 della gittata
  (187 px invece di 280), e un nemico tra i due restava fuori. L'aggancio scarta i bersagli a meno
  di 4 px dal limite (si muovono durante il frame).
  Il corridoio vicino al giocatore è un cono (un'unità di fianco non è "davanti"):

  | Arma (tipo) | Mezza larghezza | Cono vicino al giocatore |
  |---|---|---|
  | Laser, cannoni, mitragliatrice, lanciafiamme (`Direct`) | 40 px | 34° |
  | Cannone balistico, lanciamine, lanciamissili (`Ballistic`, `MineLayer`, `FixedMineLayer`, `Missile`: corridoio "magnetico") | 80 px | 46° |

  Le larghezze sono in pixel di mondo (`Settings::corridorDirect/corridorMagnetic`), moltiplicate
  per l'opzione *Larghezza corridoio di aggancio* e per lo zoom (`FrameInfo::zoom`). Il tipo
  dell'arma viene **dai dati**, non dai nomi: la sessione segue la catena di ciò che la testa
  spara (`fireChain` in `src/game/engine.hpp`: i figli di `act_fight` della testa, poi il figlio
  `act_fight` di ogni oggetto e il figlio d'impatto `act_death` di ciò che vola) e
  `weaponKind()` (`src/game/session_controls.hpp`) decide:

  | Regola (nell'ordine) | Tipo | Teste della Kassandra |
  |---|---|---|
  | nella catena c'è una mina (classe `Mine`) e l'arma ha il flag 4 (testa fissa) | `FixedMineLayer` | 205 CHAOS (la mina è posata sul posto, come nell'originale) |
  | nella catena c'è una mina | `MineLayer` | 210 PLUTO (proiettile balistico che lascia una mina) |
  | flag 1 (spara senza girare la torretta) o un `CannonMissile`/`AviaMissile` nella catena | `Missile` | 90 ZEPHYR, 204 HARPY (a ricerca), 209 HYDRA |
  | il primo proiettile ha velocità verticale ≥ orizzontale (parabola alta) | `Ballistic` | 91 HAIL (200/400) |
  | altrimenti | `Direct` | 88 HEPHEST, 202 HELIOS, 89 PHOEBUS, 203 THUNDER, 208 VULCAN, 207 PHOENIX |

  Il lanciafiamme PHOENIX (corto raggio) sta nel corridoio stretto: è un'arma diretta e veloce.
  L'aggancio usa sempre la gittata attuale dell'arma letta dal motore (`weapons[]` dei dati, che
  gli script possono cambiare con `SetVid(n, VID_WEAPONRANGE, …)`: `HEADER.INI` lo fa solo nel
  gioco in rete, `NetTact`).
  Le armi antiaeree (`targetCategory` con gli aerei: THUNDER, HARPY, HYDRA, ZEPHYR) trattano il
  punto del mouse senza unità sotto come un aereo 80 px più in alto (`act_coor_attack`, 0x411c41):
  il punto a gittata massima è alzato di 80 px (`FrameInfo::aimLift`), così il colpo arriva a
  gittata piena lungo lo stick.
- **Grilletti.** RetroArch dà la pressione di L2/R2 (`RETRO_DEVICE_INDEX_ANALOG_BUTTON`) e, sui pad
  con grilletti digitali, 0x7fff quando sono premuti; se un frontend riporta 0, conta il pulsante
  digitale come pressione piena. Zona morta 8 %. Nel cursore virtuale un grilletto clicca oltre
  metà corsa.
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
            .aimDistance = c.weaponRange, .weaponKind = c.weaponKind, .zoom = 1 /* zoom della vista */,
            .targets = c.targets};
const PcState& pc = mapper.update(pollRaw(input_state_cb, mapper.settings()), f);
// pc.cursor, pc.left/right (down/pressed/released), pc.key(Key::Num1..), pc.text, pc.move,
// pc.moveOrder/moveTarget (ordine di movimento), pc.hasAimTarget/aimTarget (punto mirato del
// diretto, anche fuori schermo), pc.cursorHidden (mirino nascosto), pc.zoomRate, pc.edgeScroll,
// pc.aimLocked, pc.weaponPending
session.setCameraFollow(...);                  // secondo l'opzione Telecamera, lo schema e il touch
session.runFrame(pc, dt);                      // InputRecordBuilder: PcState -> record originale
```

Il motore espone `Engine::playerAim()` (aggiunta compatibile al contratto, implementazione vuota
di default): posizione del flagman, gittata dell'arma in uso e unità ostili che l'arma può
colpire, con il punto che le mette "sotto il cursore" (centro del riquadro di `unitAt`), e
`PlayerAim::fire` (issue #19: testa dell'arma, flag, `targetCategory`, classi della catena di
fuoco, classe e velocità del primo proiettile) da cui `weaponKind()` ricava il tipo d'arma.
La gittata di `PlayerAim::weaponRange` è euclidea per i colpi del giocatore (vedi sopra).
Senza questa query la sessione ricava la posizione del giocatore da `Flagman`/`GetX`/`GetY`.

## Prove automatiche

- `input_mapper_test`: schemi, ordini di movimento (anche lo stesso ritmo a 60 e 120 fps),
  aggancio invisibile (il più vicino nel corridoio, categorie = bersagli dati dal motore, corridoi
  per tipo d'arma, cono vicino, fuori schermo, gittata, nessun bersaglio → gittata massima, scala e
  zoom), mirino nascosto, schema a puntamento in gioco (L2/R2 click, A/B zoom, non nei menu, nel
  nome e senza giocatore, A tenuto dal menu), zoom analogico e digitale del diretto, richieste
  d'arma ripetute, touch, mouse, tastiera, nome.
- `controls_test`: record di input con ordini, punto mirato fuori schermo e fascia dei bordi,
  tipo d'arma dalla catena di fuoco, telecamera che segue (zona morta, anticipo, 60 = 120 fps,
  16:9, pausa con lo scorrimento ai bordi), armi dallo script, indicatore dell'arma.
- `controls_data_test` (con `GROMADA_DATA`): il tipo di ogni testa della Kassandra dai dati
  originali (tabella sopra), con arma, flag e categorie.
- `controls_e2e_{pad,touch}_{60,120}fps_{43,169}` (con `GROMADA_DATA`, `tests/controls_e2e.sh`):
  attraverso il core e l'harness sul livello 1.
  *pad*: menu solo col pad, poi schema diretto: 1 s a destra e 2,5 s in su, la Kassandra si ferma
  dove si lascia lo stick (entro ~7 px tra 60 e 120 fps), la vista l'ha seguita, lo stick destro
  aggancia la torretta davanti e la colpisce (i punti vita dei bersagli scendono).
  *touch*: menu solo col touch (tocchi per filmati e titolo, pressione lunga = nuovo giocatore,
  tocco = conferma il nome, tocco su NORMALE, AVVIO), poi un tocco 180 px a destra porta lì la
  Kassandra e la vista la segue.
- `controls_lock_e2e_{lock,ballistic}_{60,120}fps_{43,169}` (con `GROMADA_DATA`,
  `tests/controls_lock_e2e.sh`): livello 1 con un profilo scritto dal test (HAIL nello slot 2);
  la Kassandra va a nord col d-pad e si ferma 212–234 px sotto una torretta T (oltre i 187 px
  della vecchia mira, entro la gittata di 280; a 60 fps T è fuori schermo).
  *lock*: twin-stick senza mirino, stick destro in su → T agganciata e colpita; schema a puntamento
  con il cursore portato su T (scorrimento ai bordi + mouse) e R2 → **gli stessi punti vita persi,
  frame per frame** (125 → 101 in sei colpi da 4); con l'aggancio spento il punto mirato è a 280 px
  (i raggi laser si fermano comunque su T a metà danno: 125 → 103 a 60 fps, → 113 a 120);
  L2 = click sinistro (la Kassandra va al cursore), A/B tenuti = zoom ±1 senza click, A nei menu
  clicca, L2/R2 analogici nel diretto = zoom 0,5 / −1 / 1 (digitale).
  *ballistic*: stick 15° a destra di T (T a 44–49 px dalla retta): HAIL aggancia T nel corridoio
  largo e i proiettili la colpiscono (125 → 80 → 35); HAIL con l'aggancio spento spara a gittata
  piena lungo lo stick e non la tocca; il laser (corridoio stretto) non la aggancia.
  I controlli usano la traccia del core: `GROMADA_TRACE=<n>` nell'ambiente scrive ogni *n* frame
  `trace map=… ctx=… flagman=x,y cam=x,y cursor=x,y weapon=… mask=… pending=… targets=… hp=…
  order=… locked=… nearest=dx,dy nhp=… mouse=x,y bits=… kind=… range=… zoom=… hidden=…`
  (`nhp` = punti vita del bersaglio più vicino, `mouse`/`bits` = record di input del frame,
  `kind` = tipo d'arma, `zoom` = `zoomRate`×100, `hidden` = mirino nascosto).

## Prove con l'harness

    tools/harness build/gromada_libretro.so <gioco>/gromada.exe 200 /tmp/shot gromada_level=5 \
        "lstick=0.6,0.3@0-40" "btn=a@70-90" "btn=b@110-130" "touch=500,400@140-180" shots=80,120,175

Input a tempo (ripetibili): `btn=<nome>@da-a`, `lstick=x,y@…`, `rstick=x,y@…`,
`touch=px,py[;px,py]@…` (dita in pixel dell'immagine), `mouse=dx,dy@…`, `mouse_btn=left|right@…`,
`l2a=p@…`, `r2a=p@…` (pressione analogica dei grilletti, 0..1).
