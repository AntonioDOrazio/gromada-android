# Gromada Installer (Android)

App Android che, partendo dall'immagine del CD originale (`.cue` + `.bin`), prepara tutto
per giocare a Gromada in RetroArch e tiene aggiornato il core. Sorgenti in `android/installer/`.
L'APK (`gromada-installer.apk`) è allegato alla release
[nightly](https://github.com/AntonioDOrazio/gromada-android/releases/tag/nightly) insieme al core.

## Uso

Prima di tutto installa **RetroArch dal sito** <https://www.retroarch.com> (per Retroid Pocket:
la versione *AArch64*, pacchetto `com.retroarch.aarch64`), aprilo una volta e concedi il permesso
di accesso ai file. Poi, nell'installer:

1. **Permesso** – concedi *Accesso a tutti i file*: l'app scrive la cartella del gioco, il core e la
   playlist in posizioni che RetroArch può leggere.
2. **Gioco** – (facoltativo) scegli la cartella di destinazione, predefinita
   `/storage/emulated/0/Gromada`; premi *Scegli immagine CD* e seleziona **insieme** il `.cue` e il
   `.bin` (pressione lunga per la selezione multipla). Se selezioni solo il `.cue`, il `.bin` viene
   cercato nella stessa cartella. Va bene anche un `.iso` (senza musica, che sta nelle tracce audio).
   L'estrazione richiede da pochi secondi a un paio di minuti.
3. **Core** – *Scarica / aggiorna core* scarica `gromada_libretro_android.so` e
   `gromada_libretro.info` dalla nightly del repository indicato (predefinito
   `AntonioDOrazio/gromada-android`) in `<cartella del gioco>/core/`.
4. **RetroArch** – *Installa il core in RetroArch* (vedi sotto come funziona), *Crea playlist
   “Gromada”*, poi *Gioca*.

Per aggiornare: all'avvio l'app controlla la nightly; se il commit è cambiato mostra
“È disponibile un core più recente”: ripeti i passi 3 e 4 (*Scarica* e *Installa il core*).
**Il gioco non va riestratto.** Se c'è una nuova versione dell'installer compare un pulsante per
scaricarla.

### Cartella del gioco prodotta

Uguale a quella dell'installer originale (stessi file e contenuti, verificato byte per byte):
`gromada.exe`, `fw.res`, `fw.ini`, `maps/` (con `levels/` e `net/`), `avi/`, `cursores/`,
`save1`…`save5`, `readme.txt`, più i file minori dell'installer (`NET.EXE`, `error*.log`, il
collegamento `.url`), e in più:

- `music/track02.flac` … `music/track12.flac`: le tracce CD audio 2–12, **senza perdita**
  (la decodifica restituisce esattamente i campioni del CD; l'MD5 del PCM è nello STREAMINFO);
- `.gromada-installer`: manifesto con origine, MD5 e dimensione di ogni file estratto;
- `core/`: core e file info scaricati dalla nightly.

Riestrarre nella stessa cartella aggiorna i file ma **non sovrascrive i salvataggi** (`saveN`).

## Cosa automatizza e cosa no (RetroArch su Android)

Analisi del codice di RetroArch (`pkg/android/phoenix`, tag v1.17.0 … v1.22.2 e `master` di
novembre 2026):

| Operazione | Come | Automatico? |
|---|---|---|
| Installare il core | `CoreSideloadActivity` (esportata, senza permessi) con l'extra `LIBRETRO` = percorso del `.so`: RetroArch lo copia in `dataDir/cores/` e si avvia con quel core | Sì, un tocco (vedi limiti) |
| Avviare il gioco | `RetroActivityFuture` con gli stessi extra del launcher di RetroArch: `ROM`, `LIBRETRO`, `CONFIGFILE`, `IME`, `DATADIR`, `APK`, `SDCARD`, `EXTERNAL` | Sì (*Gioca*) |
| Playlist | file JSON `Gromada.lpl` (formato 1.5) in `/storage/emulated/0/RetroArch/playlists` | Sì, se RetroArch usa la cartella predefinita |
| File `.info` del core | va in `dataDir/info/`, privata | **No** (non necessario) |
| Aggiornare il core | nuova copia con `CoreSideloadActivity` | Sì, un tocco |

Perché non si può fare di più:

- **I core si caricano solo dalla cartella privata di RetroArch** (`/data/user/0/<pacchetto>/cores`).
  La memoria condivisa è montata `noexec` ed è fuori dal namespace del linker; nessun'altra app può
  scrivere nella cartella privata. L'unica porta d'ingresso è `CoreSideloadActivity`, pensata per
  gli sviluppatori: copia il file e avvia RetroArch con il core **senza** passare `CONFIGFILE`, quindi
  quell'avvio usa una configurazione di ripiego. Per questo l'installer avvisa: se RetroArch si apre
  dopo “Done!”, esci da RetroArch e torna all'installer; poi usa *Gioca* o la playlist, che usano
  la tua configurazione normale. Se RetroArch era già aperto, alla richiesta si chiude da solo
  (`onNewIntent` con un core diverso → `finish()`), dopo aver copiato il core.
- **L'installer non può verificare** che il core sia stato copiato (la cartella è privata). Il
  ricevitore `com.retroarch.QUERY_INSTALLED_CORES` esiste solo su `master`, non nelle versioni
  rilasciate: per ora l'app ricorda quale versione ha inviato a RetroArch.
- **Il file `.info`** finisce in `dataDir/info`: non si può copiare. Senza, RetroArch mostra il nome
  del file al posto di “Gromada (Buka)” nella lista dei core, ma il core funziona (le informazioni
  necessarie, come `need_fullpath`, arrivano da `retro_get_system_info`). Cambiare la cartella
  *Info core* di RetroArch non conviene: nasconderebbe le info di tutti gli altri core.
- **Playlist**: RetroArch legge la cartella impostata in *Impostazioni → Cartelle → Playlist*; se
  l'hai cambiata, indica la stessa cartella con *Cambia cartella playlist*. La configurazione
  (`/storage/emulated/0/Android/data/<pacchetto>/files/retroarch.cfg`) non è leggibile da altre app
  su Android 11+, quindi l'installer non può scoprirla da solo.
- **Musica a 120 Hz / frequenza video**: l'extra `REFRESH` esiste ma l'installer non lo usa; la
  frequenza si sceglie dalle opzioni del core e di RetroArch.

### Versioni di RetroArch

| Pacchetto | Origine | Target SDK | Accesso ai file del gioco | Supporto |
|---|---|---|---|---|
| `com.retroarch.aarch64` | sito (RetroArch AArch64) | 28 | archiviazione legacy con il permesso | **Sì, consigliato** |
| `com.retroarch` | sito | 28 | come sopra | Sì |
| `com.retroarch.ra32` | sito (32 bit) | 28 | come sopra | Il core è solo arm64: no |
| `com.retroarch` | Google Play | 35 | nessun permesso di archiviazione (fino a 1.22.2) | **No** |
| `com.retroarch.aarch64` | Google Play (“RetroArch Plus”) | 35 | come sopra | **No** |

Le build di Google Play (fino alla 1.22.2) non dichiarano permessi di archiviazione: non possono
leggere né il `.so` da installare né la cartella del gioco (il core legge `fw.res`, `maps/`, ecc.
con percorsi normali, non tramite SAF). L'installer le riconosce (target SDK ≥ 30 senza
`MANAGE_EXTERNAL_STORAGE`, installer `com.android.vending`) e consiglia la versione del sito.
Il `master` attuale di RetroArch richiede `MANAGE_EXTERNAL_STORAGE`: le versioni future che lo
includono funzioneranno se a RetroArch viene concesso *Accesso a tutti i file*.

### Procedura manuale (se l'automatica non va)

1. In RetroArch: *Menu principale → Carica core → Installa o ripristina un core* → scegli
   `/storage/emulated/0/Gromada/core/gromada_libretro_android.so`.
2. *Carica contenuto* → `/storage/emulated/0/Gromada/gromada.exe` (o `fw.res`).

## Edizioni del gioco supportate

- **Italiana** (CD con `INSTALL_ITA/DATA1.HDR` + `DATA1.CAB`, InstallShield 5, 200 file; tracce
  audio 2–12): verificata con l'immagine reale.
- Altre edizioni: l'estrattore cerca nell'immagine qualunque `DATA1.HDR`/`DATA1.CAB` (InstallShield
  5 e successivi) che contenga `fw.res`, oppure una cartella con `fw.res` copiata così com'è.
  Le varianti non verificate potrebbero richiedere aggiustamenti: apri una issue con l'elenco dei
  file del CD (`java -jar gromada-extract.jar --list immagine.cue`).
- Formati d'immagine: `.cue` con un solo `.bin` o un file per traccia (`BINARY`, `MOTOROLA`,
  `WAVE`), tracce dati `MODE1/2048`, `MODE1/2352`, `MODE2/2352`, `MODE2/2336`; `.iso`; `.bin` senza
  cue (solo dati). Le tracce si tagliano ai confini `INDEX 01`. Non supportati: `.ccd/.img`, `.mds`,
  `.chd`, `.nrg`, cabinet InstallShield divisi su più volumi.

## Architettura

- `android/installer/common` (Kotlin/JVM, nessuna dipendenza): la logica, testabile sul desktop.
  - `CueSheet`, `DiscImage`: lettura del cue e taglio delle tracce.
  - `DataTrack`, `Iso9660`: settori grezzi → settori logici da 2048 byte (sync/header verificati,
    modalità letta dall'header), file system ISO 9660 con nomi Joliet.
  - `InstallShieldCab`: lettura di `DATA1.HDR`/`DATA1.CAB` (formato documentato da
    [unshield](https://github.com/twogood/unshield), MIT): tabella dei file, cartelle, gruppi, blocchi
    `[u16 lunghezza][deflate raw]`, offuscamento; verifica dimensione e MD5 di ogni file.
  - `FlacEncoder`: encoder FLAC senza perdita in puro Kotlin (predittori fissi 0–4, sottoframe
    costanti/verbatim, decorrelazione stereo left/side, right/side, mid/side, codifica Rice
    partizionata, CRC-8/CRC-16, MD5 nello STREAMINFO). Compressione circa come `flac -2`: le 11 tracce
    (254 MB di PCM) diventano ~154 MB. Scelto invece del codificatore FLAC di `MediaCodec` perché è
    deterministico, identico su desktop e Android e verificabile nei test (MediaCodec non è
    disponibile sul desktop né in CI e non scrive l'intestazione del file).
  - `GameExtractor`: individua il gioco, estrae (file `.part` rinominati a fine scrittura, quindi un
    errore o un annullamento non lascia file troncati), codifica la musica in parallelo.
  - `RetroArch`: costanti, extra degli intent e playlist (vedi sopra).
  - `Cli`: `java -jar gromada-extract.jar GROMADA.cue cartella [--bin FILE] [--no-music]`,
    `--list` per elencare il contenuto.
- `android/installer/app` (Android, minSdk 29, targetSdk 35, nessuna libreria AndroidX): interfaccia
  in italiano, utilizzabile anche con il pad. Legge le immagini tramite Storage Access Framework
  (canale del file descriptor, letture posizionali), scrive con l'accesso a tutti i file; controlla la
  release con l'API REST di GitHub (`/repos/{repo}/releases/tags/nightly`: `target_commitish` e asset).

### Firma dell'APK

L'APK è firmato con una chiave di debug **pubblica** inclusa nel repository
(`android/installer/app/debug.keystore`, password `android`), così le nightly si installano una
sopra l'altra senza disinstallare. Non è una chiave segreta: chiunque può firmare un APK
equivalente. Prima di una distribuzione più ampia va sostituita con una chiave di rilascio custodita
nei secret della CI.

## Verifica e test

- `android/installer`: `./gradlew :common:test` (CI). Test sintetici: cue, ISO 9660/Joliet e
  Mode 1 grezzo, cabinet InstallShield 5 generato nel test (compresso e non, MD5 errato), estrazione
  completa di un'immagine sintetica, annullamento, salvataggi preservati, encoder FLAC con un
  decodificatore indipendente e severo (CRC, MD5) e, se presente, `flac -t`.
- Test sull'immagine reale (solo in locale): `GROMADA_CUE=…/GROMADA.cue GROMADA_BIN=…/GROMADA.bin
  GROMADA_DATA=…/cartella-di-riferimento ./gradlew :common:test` confronta ogni file con
  l'installazione di riferimento e ogni traccia con l'audio grezzo del CD. Risultato sull'edizione
  italiana: 200/200 file identici, tracce 2–12 identiche campione per campione (verificato anche con
  `flac -t` e decodificando con ffmpeg).
- Core: `tests/music_test` (FLAC/WAV sintetici, OGG di prova) nella CI Linux.
