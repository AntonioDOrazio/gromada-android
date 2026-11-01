# Filmati (`avi/0.avi` … `avi/3.avi`)

Il core riproduce i filmati originali del gioco leggendoli dalla cartella `avi/` della copia
dell'utente. Codice in `src/video/` (header-only), sorgente audio in streaming in
`src/engine/audio_stream.hpp` + `gromada::Mixer::setStream`, integrazione nel core in
`src/gromada_libretro.cpp` (blocco "movies").

## I file del gioco

| File    | Video                    | Frame (codificati) | Durata video | Audio                        | Durata audio |
|---------|--------------------------|--------------------|--------------|------------------------------|--------------|
| `0.avi` | MS Video 1 (`CRAM`) 16 bit | 235 (235)        | 15,67 s      | PCM u8 stereo 22 050 Hz      | 15,67 s      |
| `1.avi` | Indeo 3 (`IV32`)         | 231 (231)          | 15,40 s      | idem                         | 15,40 s      |
| `2.avi` | Indeo 3 (`IV32`)         | 2772 (297)         | 184,80 s     | idem                         | 186,32 s     |
| `3.avi` | Indeo 3 (`IV32`)         | 1176 (1176)        | 78,40 s      | idem                         | 96,00 s      |

Tutti 640×480 a 15 fps. Particolarità utili per chi tocca il codice:

- **Struttura RIFF**: `hdrl` (avih + 2 `strl`), `movi`, `idx1` (offset relativi al fourcc `movi`),
  più un `LIST PRMI` finale ignorato. Audio con `dwInitialFrames = 11`: il primo chunk audio
  (1 s, 44 100 byte) precede il primo frame. L'interleaving non conta: i chunk si leggono per offset.
- **Frame vuoti**: in `2.avi` 2475 chunk `00dc` su 2772 hanno dimensione 0 ("drop frame": resta
  l'immagine precedente; è quasi una sequenza di immagini fisse).
- **`00db`**: in `2.avi` e `3.avi` i keyframe Indeo sono nei chunk `00db`, gli altri in `00dc`:
  il parser tratta entrambi come video.
- **Audio più lungo del video**: `3.avi` ha 17,6 s di colonna sonora (a basso volume) dopo
  l'ultimo frame, che è nero; `2.avi` 1,5 s. Il filmato finisce quando finisce il flusso più lungo
  (l'ultima immagine resta a schermo), come nei player comuni.
- Indeo 3: tutti i frame hanno versione 32, `cb_offset` 0, la stessa tabella `alt_quant`, nessun
  frame di sincronia; flag ricorrenti `0x5`/`0xd` (keyframe) e `0x300`, `0x200`, `0x108` (inter;
  il bit 9 sceglie il buffer).

## Moduli

- `avi.hpp` — `AviFile`: header (`avih`, `strh`, `strf` con BITMAPINFOHEADER + palette o
  WAVEFORMATEX), indice da `idx1` (offset relativi o assoluti, verificati sul primo e
  sull'ultimo chunk) con ripiego sulla scansione di `movi` (anche liste `rec ` e RIFF `AVIX`).
  I chunk si leggono su richiesta: la memoria non dipende dalla durata del filmato.
- `msvideo1.hpp` — decoder MS Video 1, 8 bit (palette) e 16 bit (RGB555).
- `indeo3.hpp` + `indeo3_tables.hpp` — decoder Indeo 3.
- `movie.hpp` — `Movie`: sceglie il decoder dal fourcc, converte in XRGB8888, gestisce il
  tempo, espone la colonna sonora come `gromada::AudioStream`, disegna nel framebuffer.

### MS Video 1

Blocchi 4×4 da sinistra a destra, righe di blocchi dal **basso** (ordine DIB). Ogni blocco
inizia con una parola a 16 bit little-endian:

- `0x84xx`–`0x87xx`: salta `parola − 0x8400` blocchi (restano quelli del frame precedente);
- 16 bit: bit 15 a 0 → 2 colori (`flags` = la parola, bit 0 = pixel in basso a sinistra, righe dal
  basso) o 8 colori se il primo colore ha il bit 15 (una coppia per quadrante 2×2: in basso a
  sinistra, in basso a destra, in alto a sinistra, in alto a destra); bit 15 a 1 → un colore;
- 8 bit: byte alto < `0x80` → 2 colori, ≥ `0x90` → 8 colori, altrimenti 1 colore (byte basso).

Un bit di `flags` a 1 sceglie il **primo** colore della coppia.

### Indeo 3

Ogni frame: header "OS" di 16 byte (numero di frame, una parola, checksum = xor degli altri tre
e di `'FRMH'`, dimensione), header del bitstream di 32 byte (versione 32, flag, dimensione in
bit, `cb_offset`, altezza, larghezza, offset dei piani Y, V, U relativi a questo header) e la
tabella `alt_quant` di 16 byte. Immagini YUV 4:1:0 (crominanza 1/4 × 1/4, 160×120) con campioni a
7 bit; in uscita si moltiplicano per 2. Due buffer: il bit 9 dei flag dice in quale si decodifica,
l'altro è il riferimento per la compensazione del moto. Un frame con dimensione dati 16 è di
sincronia (si ripete l'immagine).

Ogni piano: numero di vettori di moto (u32, ≤ 256), i vettori (coppie `dy`, `dx` di byte con
segno), poi un albero binario di celle (unità 4×4 pixel) con codici a 2 bit letti dal bit più
significativo:

- `0`/`1`: divisione orizzontale / verticale (metà arrotondata a numero pari: `((n+2)>>2)<<1`;
  in verticale prima si divide in strisce larghe 40 unità per la luminanza, 10 per la crominanza);
- nell'albero "moto": `2` = cella INTRA, `3` = cella INTER (il byte successivo dei dati è l'indice
  del vettore); poi si passa all'albero "VQ";
- nell'albero "VQ": `2` = cella NULL (altri 2 bit, 0 o 1: la cella si copia dal riferimento,
  spostata del vettore o ferma se INTRA), `3` = cella codificata.

I codici dell'albero e i byte delle celle condividono un solo cursore: un byte dell'albero si
consuma tutto quando se ne legge il primo codice, i dati delle celle seguono.

Una cella codificata inizia con un byte: modo (nibble alto) e codebook (nibble basso,
+`cb_offset`; nei modi 1 e 4 indica invece una voce di `alt_quant` con due codebook, primario per
le righe dispari e secondario per le pari). Modi: `0`/`1` blocchi 4×4; `3`/`4` blocchi 4×8 (solo
INTRA: si codificano le righe dispari, le pari sono la media con la riga sopra); `10` blocchi 8×8
(INTRA: righe e colonne a metà risoluzione; INTER: delta sul blocco già copiato col moto); `11`
blocchi 4×8 (solo INTER). Per ogni blocco 4 codici, uno per riga (o coppia di righe):

- `< numDyads`: "dyad", due byte: il primo è il delta della coppia di pixel destra, il secondo
  della sinistra;
- da `numDyads` a 247: "quad", un byte con entrambi gli indici (`q / base`, `q % base`;
  scambiati nei codebook 16–23);
- `0xFD`/`0xFE`/`0xFF`: copia le righe fino alla 4ª/3ª/2ª del blocco; `0xFC`: come `0xFD` e il
  blocco successivo invariato; `0xFB n`: resto del blocco + `n & 31 − 1` blocchi invariati (bit 5:
  "salta", nelle celle INTRA non copia nulla); `0xFA`/`0xF9`: blocco (e il successivo) saltato.

I delta si sommano alla riga sopra (INTRA) o al riferimento (INTER) come parole a 16/32 bit con
`& 0x7F7F…`: un delta negativo sul pixel sinistro prende in prestito dal destro, come nelle
implementazioni di riferimento. Con codebook ≥ 8 la riga di predizione viene prima
riquantizzata **sul posto** (anche nell'immagine di riferimento, per le celle INTER).

**Tabelle.** I 24 codebook sono la "seed area" del codec Intel (`IR32_32.DLL`, `.data 0x1004d26a`):
per ogni tabella `count`, `count` coppie di delta con segno (pixel sinistro, destro), un byte
`expand` (|expand| = base dei quad, negativo = indici scambiati). La tabella di riquantizzazione
(8 sezioni × 128) è `(j + off[i]) / (i+2) * (i+2) + d[i]` con `off = {1,1,2,−3,−3,3,4,4}`,
`d = {0,1,0,4,4,1,0,1}` più 11 eccezioni, verificate voce per voce sulla tabella Intel.

## Player

- **Tempo**: `Movie::step(dt)` mostra il frame `floor(t · 15)` e poi avanza `t` di `dt`; il core
  lo chiama una volta per frame di uscita con `dt = 1/60` o `1/120`. L'audio mixato nello stesso
  frame copre `[t, t + dt)`: l'immagine a schermo è sempre quella il cui intervallo contiene
  l'audio in riproduzione (scarto audio−video tra 0 e 50 ms a 60 fps, 0–58 ms a 120 fps, sempre
  < 1 frame video = 66,7 ms). I frame intermedi si decodificano tutti (servono come riferimento),
  ma si converte in RGB solo l'ultimo.
- **Audio**: PCM 8/16 bit mono/stereo letto chunk per chunk, convertito in int16 stereo; il mixer
  lo ricampiona linearmente a 44 100 Hz (22 050 → 44 100: passo esatto 0,5, nessuna deriva) e lo
  mixa a volume pieno. `Mixer::streamPosition()` è l'orologio audio usato dai test.
- **Colore**: Indeo 3 → RGB con coefficienti BT.601 a gamma limitata (come ffmpeg per questi
  file) e crominanza interpolata bilinearmente con campioni al centro del blocco 4×4;
  RGB555 → RGB888 replicando i bit alti (31 → 255), identico a ffmpeg.
- **Schermo**: l'immagine si scala nel framebuffer mantenendo le proporzioni; 640×480 riempie il
  4:3 e in 16:9 (854×480) ha bande nere laterali di 107 pixel.

## Nel core

Opzione **"Riproduci filmato"** (`gromada_movie`: `nessuno`, `0`…`3`): scegliendo un filmato
parte subito; se è impostata al caricamento del gioco, il filmato parte all'avvio. **Start, A o B**
lo saltano. Durante il filmato la musica è muta e il viewer è fermo; alla fine si torna al
livello. Il log riporta durata in frame di uscita ed eventuali errori di decodifica.

## Verifiche

- `tests/video_test` (anche in CI, senza dati): AVI sintetici (con `idx1`, senza, con liste
  `rec `, frame vuoti), blocchi MS Video 1 costruiti a mano (16 e 8 bit), un frame Indeo 3 costruito
  a mano (dyad, RLE, cella NULL e cambio di buffer), tabelle, sorgente in streaming del mixer,
  sincronia e durata del player a 60 e 120 fps su un AVI sintetico con audio "marcato" col
  numero del frame.
- `tests/video_data_test` (con `GROMADA_DATA` e ffmpeg nel `PATH`): decodifica completa dei 4
  filmati, confronto con ffmpeg di **ogni** frame codificato, PSNR RGB ogni 10 frame, riproduzione
  a 60 e 120 fps con controllo di durata e sincronia a ogni frame di uscita.

Risultati (desktop x86-64, clang 18 -O3, un core):

| File    | Uscita del decoder vs ffmpeg      | PSNR RGB vs ffmpeg (media / min) | ms/frame decodifica | + conversione RGB |
|---------|-----------------------------------|----------------------------------|---------------------|-------------------|
| `0.avi` | identica, 235/235 frame (RGB555)  | identico (∞)                     | 0,13                | 0,28              |
| `1.avi` | identica, 231/231 (YUV 4:1:0)     | 44,5 / 42,1 dB                   | 0,22                | 1,34              |
| `2.avi` | identica, 297/297                 | 47,7 / 45,9 dB                   | 0,18                | 1,27              |
| `3.avi` | identica, 1176/1176               | 44,5 / 40,6 dB                   | 0,19                | 1,49              |

Il PSNR RGB < ∞ per Indeo 3 viene solo dalla conversione colore (ffmpeg interpola la crominanza
con un filtro bicubico); i piani YUV sono identici. Durate: 940/1880 frame di uscita per `0.avi`
a 60/120 fps (15,667 s), 924/1848 per `1.avi`, 11 180/22 359 per `2.avi` (fine dell'audio),
5761/11 521 per `3.avi`; 0 errori di sincronia. Nota: con `-fps_mode cfr` ffmpeg anticipa di un
frame 4 immagini di `2.avi` dopo i frame vuoti; il confronto usa quindi `passthrough` (solo frame
codificati), mentre il player rispetta l'indice dei chunk.

Prova end-to-end col frontend headless:

    tools/harness build/gromada_libretro.so /percorso/gioco/gromada.exe 960 /tmp/m0 \
        gromada_movie=0 gromada_fps=60 shots=400

Lo screenshot 400 coincide col frame 100 di ffmpeg e l'audio catturato è la colonna sonora
ricampionata, senza ritardo.

## Provenienza e licenze

- `avi.hpp`, `msvideo1.hpp`, `movie.hpp`: scritti per questo progetto (MIT) dalla descrizione
  pubblica dei formati (RIFF/AVI di Microsoft, MultimediaWiki per MS Video 1).
- `indeo3.hpp`: scritto per questo progetto senza copiare codice, ma **l'algoritmo segue quello
  del decoder di FFmpeg** (`libavcodec/indeo3.c`, LGPL-2.1+), comprese le stranezze riprodotte per
  avere un'uscita identica (riquantizzazione sul posto, codici RLE "salta" nelle celle INTRA): è la
  descrizione più completa del formato e durante lo sviluppo la pagina MultimediaWiki non era
  raggiungibile. **Da valutare** se considerarlo derivato: in quel caso basta intestarlo LGPL-2.1+
  (è già un file a sé, usato solo da `movie.hpp`), oppure compilare con `-DGROMADA_NO_INDEO3`
  per escluderlo (i filmati 1–3 non partono, con un messaggio nel log). FFmpeg è servito anche
  come riferimento a scatola chiusa per i confronti.
- `indeo3_tables.hpp`: i byte della seed area Intel sono copiati da
  [oxideav-indeo](https://github.com/OxideAV/oxideav-indeo) (MIT, © 2026 Karpelès Lab Inc.;
  licenza riportata in testa al file). In origine sono dati del codec Intel, come le tabelle
  equivalenti di FFmpeg.

## Limiti noti

- Cambi di palette (`##pc`) dei filmati MS Video 1 a 8 bit non applicati (i file del gioco sono a
  16 bit).
- Indeo 3: frame a 8 bit per pixel e vettori a mezzo pixel non supportati (non usati dal gioco,
  e rifiutati anche da ffmpeg); un cambio di dimensione a metà filmato non viene mostrato.
- Audio: solo PCM 8/16 bit.
