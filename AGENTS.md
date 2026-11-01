# Guida per chi lavora sul repo (persone e agenti)

## Il progetto
Core libretro che **reimplementa** il motore di Gromada (Buka, 1999) leggendo i dati originali.
Target: Android (Retroid Pocket Nova 4:3 120 Hz, Flip 2 16:9 60 Hz) con RetroArch in Vulkan e shader slang.
Roadmap e scoperte: `README.md`, `docs/`. Architettura del gioco e contratto tra i moduli: `docs/architecture.md`.

## Regole sui dati del gioco
- **Mai** committare file del gioco (fw.res, mappe, script `.INI`, `.men`, testi, video, musica, exe),
  né loro estratti significativi (sprite, testi di missione, audio). Sono protetti da copyright.
- Per test e sviluppo locale la cartella del gioco è indicata da `GROMADA_DATA`
  (contiene `gromada.exe`, `fw.res`, `maps/`, `avi/`, `cursores/`, `music/trackNN.ogg`).
- Nella documentazione di reverse engineering si possono citare nomi, offset, strutture e
  brevi frammenti necessari a spiegare un formato o un comportamento, non interi file.

## Architettura del codice
- C++23, clang ≥ 18 + libc++ su desktop; Android NDK r27+ (`ANDROID_STL=c++_static`). Niente moduli C++.
- `src/libgromada`, `src/core`: formati del gioco, portati da gromada-viewer (MIT).
  Gli header **contengono le implementazioni**: si includono una sola volta per binario.
- Il core è **una sola unità di traduzione C++** (`src/gromada_libretro.cpp`) più eventuali unità C
  di terze parti (`src/third_party`). I nuovi moduli sono header-only in `src/<modulo>/`.
- Un modulo nuovo deve dipendere solo da `core/`, `libgromada/` e da interfacce esplicite: niente
  dipendenze incrociate tra moduli in sviluppo parallelo. L'integrazione in `gromada_libretro.cpp`
  deve restare **minima e isolata** (poche righe, facili da fondere).
- Terze parti: solo librerie single-file con licenza permissiva (MIT, BSD, zlib, pubblico dominio),
  in `src/third_party/` con la licenza in testa al file. Codice LGPL solo se concordato, in un file
  separato e documentato.
- Il core deve restare compatibile con 60 e 120 fps (tempo passato in secondi, nessuna assunzione sul frame rate).

## Test
- `cmake -S . -B build -G Ninja -DGROMADA_BUILD_TESTS=ON && cmake --build build && (cd build && ctest)`
- Un test = un eseguibile (`tests/<nome>_test.cpp`, registrato con `gromada_add_test` in `tests/CMakeLists.txt`).
- Helper in `tests/test.hpp`: `CHECK`, `CHECK_EQ`, `SKIP_WITHOUT_DATA()` (uscita 77 = saltato).
  La CI non ha i dati del gioco: i test che li richiedono vengono saltati lì, ma vanno eseguiti in locale.
- Frontend headless per prove end-to-end: `tools/harness.cpp` (screenshot PPM, cattura audio WAV, input scriptato).

## Flusso git
- Un branch per issue: `issue-<numero>-<slug>`. Commit piccoli e descrittivi, messaggi in inglese.
- Non si committa su `main` direttamente: l'orchestratore fonde i branch, compila, esegue i test e pubblica.
- Ogni push su qualunque branch avvia la CI (build Linux + Android, test). Solo `main` pubblica la release nightly.
- Documentazione nuova in `docs/` (italiano).
