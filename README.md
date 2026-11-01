# Gromada — core libretro

Reimplementazione del motore di **Gromada** (Buka Entertainment, 1999) come core libretro,
pensata per Android (Retroid Pocket Nova / Flip 2) con RetroArch in Vulkan e shader slang.

Il core **non contiene asset del gioco**: legge i file della tua copia originale.

## Stato: M2 — viewer + audio
- Carica `fw.res` e i 30 livelli originali, li disegna con il renderer software
  (sprite indicizzati, ombre, luce, trasparenze) e li fa scorrere col pad.
- Opzioni core: **Frame rate 60/120**, **Formato 4:3 (640×480) / 16:9 (854×480)**, livello di anteprima.
- Opzioni audio: **Volume musica** e **Volume effetti**.
- Comandi (dettagli e motivazioni in `docs/controls.md`): cursore virtuale o schema diretto
  (twin-stick) da opzione core, touchscreen (tocco = click sinistro, pressione lunga o due dita =
  click destro), mouse, cursori originali `.CUR`. A = click sinistro, B = click destro,
  L1/R1 = armi, Start = Esc, Select = TAB. Nel viewer: d-pad o cursore ai bordi = scorrimento,
  Y = veloce, L2/R2 = livello, L3/R3 = prova effetti sonori.
- Audio: mixer interno, sincronizzato al campione a 60 e 120 fps.
  - Effetti dal `fw.res` (WAV mono 8 bit 11/22 kHz, ricampionati a 44,1 kHz).
  - Musica: le tracce CD Audio originali (2–12) in OGG, con la playlist di ogni livello
    letta dal suo script (`PlayMusic(m1,m2,m3,m4)`), ciclata come nel gioco originale.
- Filmati: opzione **Riproduci filmato** (`avi/0.avi` … `3.avi`, MS Video 1 e Indeo 3, audio
  sincronizzato a 60 e 120 fps; Start/A/B per saltare). Dettagli in [`docs/video.md`](docs/video.md).

## Preparare i file del gioco
**Su Android** usa l'app **Gromada Installer** (`gromada-installer.apk` nella release
[nightly](https://github.com/AntonioDOrazio/gromada-android/releases/tag/nightly)): dall'immagine
del CD (`.cue` + `.bin`) estrae il gioco e la colonna sonora in FLAC, scarica il core, lo installa
in RetroArch, crea la playlist e lo tiene aggiornato. Dettagli e limiti di RetroArch su Android:
[docs/installer.md](docs/installer.md). Sul desktop la stessa logica è in `gromada-extract.jar`:

    java -jar gromada-extract.jar GROMADA.cue cartella-del-gioco [--bin GROMADA.bin]

A mano: serve la cartella installata del gioco con `fw.res` e `maps/`. In RetroArch carica come
contenuto `gromada.exe` (o `fw.res`) da quella cartella.

La colonna sonora è su CD Audio, quindi l'installer originale non la copia: le tracce 2–12 del CD
vanno in `music/track02.flac` … `music/track12.flac` dentro la cartella del gioco (formati letti,
in quest'ordine: `.flac`, `.ogg`, `.wav`). Con un'immagine `.bin/.cue`, per esempio:

    # traccia N dal cue: tempi di inizio/fine in settori, 2352 byte per settore
    ffmpeg -f s16le -ar 44100 -ac 2 -ss <inizio_s> -t <durata_s> -i GROMADA.bin track02.flac

Senza questi file il gioco funziona lo stesso, solo senza musica.

## Build
Desktop Linux (clang ≥ 18 + libc++):

    CXX=clang++-18 cmake -S . -B build -G Ninja && cmake --build build

Android arm64 (NDK r27+):

    cmake -S . -B build-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
    cmake --build build-android

Il workflow `.github/workflows/build.yml` compila il core a ogni push e pubblica `gromada_libretro_android.so` nella release **nightly**: https://github.com/AntonioDOrazio/gromada-android/releases/tag/nightly

Su Android l'installer installa il core in RetroArch; a mano: *Carica core → Installa o
ripristina un core* e scegli il `.so`.

L'APK dell'installer (`android/installer`, Gradle) si compila con l'Android SDK:
`cd android/installer && ./gradlew :common:test :app:assembleRelease`.

## Test senza RetroArch

    clang++-18 -std=c++20 tools/harness.cpp -o tools/harness -ldl
    tools/harness build/gromada_libretro.so /percorso/gioco/gromada.exe 240 shot \
        gromada_level=5 gromada_fps=120 hold=right shots=0,239

Input a tempo per l'harness (stick, touch, mouse): vedi `docs/controls.md`.

## Struttura
- `src/libgromada`, `src/core`: lettura dei formati di Gromada, portata da
  [gromada-viewer](https://github.com/allcreater/gromada-viewer) (MIT, commit 3fb84f4)
  convertendo i moduli C++23 in header (`tools/demodularize.py`), perché `import std`
  non è ancora utilizzabile con l'NDK. Gli header contengono le implementazioni:
  il core è compilato come singola unità di traduzione.
- `src/engine`: livello runtime, renderer e mixer audio.
- `src/script`: interprete degli script `.INI` del gioco (vedi `docs/script-language.md`).
- `src/input`: schemi di controllo, cursore virtuale, touch/mouse/tastiera, cursori `.CUR`.
- `src/video`: demuxer AVI, decoder MS Video 1 e Indeo 3, player dei filmati.
- `src/third_party/stb_vorbis.c`: decoder OGG (pubblico dominio / MIT);
  `dr_flac.h`, `dr_wav.h`: decoder FLAC e WAV (pubblico dominio / MIT-0).
- `android/installer`: app Android di installazione e CLI desktop (Kotlin), vedi `docs/installer.md`.
- `src/gromada_libretro.cpp`: API libretro.

## Licenze
Il progetto è MIT (`LICENSE.txt`). Eccezione: `src/video/indeo3.hpp` (decoder Indeo 3) segue da vicino
l'algoritmo del decoder di FFmpeg ed è quindi distribuito come LGPL-2.1-or-later; si può escludere
compilando con `-DGROMADA_NO_INDEO3` (i filmati 1–3 non verranno riprodotti). Le librerie in
`src/third_party/` riportano la propria licenza in testa al file.
