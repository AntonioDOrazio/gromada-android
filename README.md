# Gromada — core libretro

Reimplementazione del motore di **Gromada** (Buka Entertainment, 1999) come core libretro,
pensata per Android (Retroid Pocket Nova / Flip 2) con RetroArch in Vulkan e shader slang.

Il core **non contiene asset del gioco**: legge i file della tua copia originale.

## Stato: M1 — "viewer"
- Carica `fw.res` e i 30 livelli originali, li disegna con il renderer software
  (sprite indicizzati, ombre, luce, trasparenze) e li fa scorrere col pad.
- Opzioni core: **Frame rate 60/120**, **Formato 4:3 (640×480) / 16:9 (854×480)**, livello di anteprima.
- Controlli: stick sinistro o d-pad = scorrimento, Y = veloce, L/R = livello precedente/successivo.
- Audio: per ora silenzio (sincronizzato correttamente a 60 e 120 fps).

## Preparare i file del gioco
Serve la cartella installata del gioco con `fw.res` e `maps/`. In RetroArch carica come contenuto
`gromada.exe` (o `fw.res`) da quella cartella.

## Build
Desktop Linux (clang ≥ 18 + libc++):

    CXX=clang++-18 cmake -S . -B build -G Ninja && cmake --build build

Android arm64 (NDK r27+):

    cmake -S . -B build-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
    cmake --build build-android

Il workflow `.github/workflows/build.yml` produce automaticamente `gromada_libretro_android.so`.

Su Android copia il `.so` nella cartella dei core di RetroArch (o usa *Installa core* dal menu)
e `gromada_libretro.info` nella cartella info.

## Test senza RetroArch

    clang++-18 -std=c++20 tools/harness.cpp -o tools/harness -ldl
    tools/harness build/gromada_libretro.so /percorso/gioco/gromada.exe 240 shot \
        gromada_level=5 gromada_fps=120 hold=right shots=0,239

## Struttura
- `src/libgromada`, `src/core`: lettura dei formati di Gromada, portata da
  [gromada-viewer](https://github.com/allcreater/gromada-viewer) (MIT, commit 3fb84f4)
  convertendo i moduli C++23 in header (`tools/demodularize.py`), perché `import std`
  non è ancora utilizzabile con l'NDK. Gli header contengono le implementazioni:
  il core è compilato come singola unità di traduzione.
- `src/engine`: livello runtime e renderer.
- `src/gromada_libretro.cpp`: API libretro.
