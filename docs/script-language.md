# Il linguaggio degli script di Gromada

Gli script `.INI` del gioco (menu, avvio missioni, condizioni di vittoria, eventi) sono scritti in
un piccolo dialetto del C. Questo documento descrive il linguaggio come lo implementa il modulo
`src/script/` (lexer, preprocessore, parser, linker e interprete ad albero), le scelte fatte dove
il comportamento originale non è ancora noto e i punti aperti per la issue #3 (reverse engineering
del runtime).

## I file

- 42 file in `maps/` (CP866/CP1251/CP1252, righe CRLF, circa 3.800 righe in tutto):
  - **frammenti inclusi**: `HEADER.INI` (builtin, costanti, variabili condivise, funzioni comuni
    come `Tact()`, `Win()`, `Lose()`, i menu di gioco), `LOCALE.INI` (stringhe localizzate),
    `music.ini` (la funzione utente `PlayMusic`);
  - **script con `main()`** (39): `logo.ini`, `mainmenu.ini`, `PLAYER.INI`, `MAPS.INI`, `NET.INI`,
    `win.ini`, `credits.ini`, `levels/01.INI`…`levels/30.INI` (`18.ini` minuscolo),
    `net/05.INI`, `net/12.INI`.
- Ogni script con `main()` comincia con `#include "maps\header.ini"`: i percorsi sono DOS,
  relativi alla cartella del gioco, con maiuscole/minuscole libere (`HEADER.INI` su disco).

## Uso del modulo

```cpp
#include "script/script.hpp"
using namespace gromada::script;

FileSystemSourceLoader loader{gameDir};                 // risolve "maps\header.ini" → maps/HEADER.INI
Script script = Script::compile(loader, "maps/levels/05.INI");   // preprocessa, analizza, collega
MyHost host;                                            // implementa ScriptHost::callBuiltin
Interpreter vm{script, host};                           // alloca le globali, esegue gli inizializzatori
vm.inheritGlobals(scriptPrecedente.snapshot());         // facoltativo: stato passato tra script
vm.tick();                                              // a ogni tick logico: esegue main() una volta
```

- `ScriptHost::callBuiltin(const BuiltinInfo&, std::span<const Value>)` riceve ogni chiamata a una
  builtin: il motore smista su `BuiltinInfo::id` (enum `BuiltinId` in `builtin_ids.hpp`).
  Gli argomenti arrivano come valori (`int` o `string`) così come sono: `argInt()`/`argString()`
  convertono. Il valore restituito diventa il valore dell'espressione.
- `ScriptHost::warning()` riceve le anomalie non fatali (indici fuori dai limiti).
- `RecordingHost` (`recording_host.hpp`) registra tutte le chiamate; `SimulatedWorld` gli fornisce
  risposte plausibili (orologio, handle per `Create`, `Flagman`, `Random` deterministico).
- Tutti gli errori sono `ScriptError` con fase (`load`, `lex`, `preprocess`, `parse`, `link`,
  `runtime`) e posizione `file:riga:colonna`.

## Lessico

- Spazi: spazio, tab, CR, LF, FF, VT. Il byte 0x1A (fine file DOS) termina l'input.
- Commenti `// …` e `/* … */` (non annidati). Un commento conta come un blank: un a-capo al suo
  interno non fa iniziare una nuova riga ai fini delle direttive (come in C).
- Fuori da commenti e letterali il testo deve essere ASCII; dentro commenti e stringhe ogni byte
  è ammesso e le stringhe restano byte grezzi (nessuna conversione di code page).
- Identificatori `[A-Za-z_][A-Za-z0-9_]*`, sensibili alle maiuscole.
  Parole chiave: `if else iff for while do return break continue int string extern`.
- Numeri: decimali, esadecimali `0x777`, ottali con lo `0` iniziale (regola del C, `strtol` base 0;
  nel gioco non ce ne sono). Valori fino a 0xFFFFFFFF, interpretati in complemento a due.
- Caratteri `'a'`, `' '`, `'\n'`: valore del byte, senza segno (0–255).
- Stringhe `"…"` con gli escape del C (`\\ \" \' \n \t \r \0 \ooo \xHH`; un escape ignoto vale il
  carattere stesso). Stringhe adiacenti si concatenano. Il gioco usa solo `\\`:
  `"maps\\levels\\01.map"` ha le cifre agli indici 12 e 13.
- **Eccezione**: il nome dopo `#include` è preso alla lettera, come un *header-name* del C:
  `#include "maps\header.ini"` ha un solo backslash.

## Preprocessore

- Le direttive iniziano una riga. Ammesse: `#include "file"`, `#define NOME corpo…`,
  `#undef NOME`. Il corpo di `#define` arriva a fine riga (commenti esclusi) ed è espanso
  pigramente nel punto d'uso, ricorsivamente, senza espandere un nome dentro la propria espansione.
- Ridefinire una macro è ammesso (l'header definisce due volte `V_M_FIGHTER`); vince l'ultima e,
  se il valore cambia, il compilatore emette un warning.
- Non supportati (assenti nel gioco, quindi errore): macro con parametri, `#if`/`#ifdef`, altre
  direttive. Le inclusioni ricorsive sono un errore; profondità massima 16.

## Grammatica

```
programma   := { extern | dichiarazione | funzione | ';' }
extern      := 'extern' ['int'|'string'] NOME '(' [parametri] ')' ['-'] NUMERO ';'
dichiarazione := ('int'|'string') declaratore { ',' declaratore } ';'
declaratore := NOME [ '[' [costante] ']' ] [ '=' (espressione | '{' espr {',' espr} [','] '}') ]
funzione    := ['int'|'string'] NOME '(' [parametri] ')' ( blocco | ';' )
parametri   := parametro { (','|';') parametro }
parametro   := ('int'|'string') [NOME] | NOME          // NOME da solo: int (in extern: tipo libero)
istruzione  := blocco | ';' | espressione ';'
             | 'if' '(' espr ')' istruzione [ 'else' istruzione ]
             | 'iff' '(' espr ')' istruzione
             | 'for' '(' [espr] ';' [espr] ';' [espr] ')' istruzione
             | 'while' '(' espr ')' istruzione
             | 'do' istruzione 'while' '(' espr ')' ';'
             | 'break' ';' | 'continue' ';' | 'return' [espr] ';'
blocco      := '{' { istruzione } '}'
```

Le espressioni seguono operatori, precedenze e associatività del C: assegnamento e assegnamenti
composti (`= += -= *= /= %= &= |= ^= <<= >>=`, a destra), `?:`, `|| && | ^ &`, `== !=`,
`< <= > >=`, `<< >>`, `+ -`, `* / %`, unari `- + ! ~ ++ --`, postfissi `++ --`, indicizzazione
`nome[espr]` e chiamata `nome(argomenti)`. Si indicizzano e si chiamano solo nomi. Niente operatore
virgola. Il `else` si lega all'`if` più vicino: il gioco scrive `else;` per chiudere un `if` interno.

Note:
- I parametri si separano con `;` (`NextMap(string nextmapname;int nextmapeffect)`) o con `,`.
- In `extern FWrite(int file,variable) 111;` `variable` è un parametro senza tipo.
- Le dichiarazioni dentro le funzioni non sono ammesse (nel gioco ce n'è una sola, commentata):
  errore esplicito invece di indovinarne la semantica.
- `iff (…) … else …` è rifiutato: il significato di un `else` dopo `iff` è ignoto.
- **Compatibilità**: `if(!unit=GetObject(…))` (`levels/01.INI`) non è C valido; il ramo `else`
  usa `unit`, quindi l'intento è `!(unit=GetObject(…))`. Il parser applica un operatore unario
  prefisso, il cui operando è assegnato, all'intero assegnamento.

## Tipi e valori

- `int`: 32 bit in complemento a due, aritmetica modulare (nessun comportamento indefinito).
  `/` e `%` troncano verso zero; **`x/0` e `x%0` valgono 0** (l'originale avrebbe un'eccezione
  della CPU); `INT_MIN/-1` = `INT_MIN`; il conteggio degli shift è mascherato a 5 bit, `>>` è
  aritmetico.
- `string`: sequenza di byte. Confronti `== != < <= > >=` tra due stringhe come `strcmp`
  (byte senza segno).
- `+` con almeno un operando stringa **concatena** (l'intero in decimale); gli altri operatori
  convertono a intero. Un confronto misto stringa/intero confronta interi.
- Conversioni: stringa → intero come `atoi` (spazi, segno, cifre; altrimenti 0); intero → stringa
  in decimale. Si applicano assegnando a una variabile o passando un argomento a un parametro di
  tipo diverso (`filename=noplayer=GetNetInfo(NET_NOPLAYER)` in `NET.INI` assegna un intero a una
  stringa).
- Verità: intero ≠ 0, stringa non vuota.
- Array `int a[3]={1,1,1};`, `string s[4];`: gli elementi non inizializzati valgono 0 / `""`.
  La dimensione è una costante intera (anche un'espressione costante) o si deduce
  dall'inizializzatore (`int a[]={…}`).
- **Nome di array senza indice = elemento 0**: il gioco dichiara `int nweapon[4]` e
  `invisible[4]` ma scrive `nweapon=GetKey()-'1'`, `weapon[nweapon]`, `invisible=GetTime()`;
  `MAPS.INI` scrive `dirsecondmenu=net_kill`. Una variabile scalare indicizzata è un array di 1.
- Indice fuori dai limiti (politica `OutOfRangePolicy::Warn`, predefinita): la lettura dà 0/`""`,
  la scrittura è ignorata, `ScriptHost::warning` viene chiamato. Con `Error` è un errore di runtime.
  (L'originale leggerebbe/scriverebbe la memoria adiacente.)
- **Stringhe come stringhe C**: `s[i]` su una `string` scalare legge il byte `i` (il terminatore,
  `i == lunghezza`, vale 0). `s[i]=c`: con `c == 0` tronca (`player_name[i]=0` in
  `PLAYER.INI`), con `i == lunghezza` accoda, oltre la fine è ignorato con warning (in C i byte in
  mezzo sarebbero indefiniti); un valore stringa assegnato a un carattere ne usa il primo byte.

## Semantica

### Variabili e parametri
- Tutte le variabili sono globali e statiche; si possono usare prima della dichiarazione (il
  linker risolve i nomi dopo aver letto tutto il programma). Ridichiarare una variabile con lo
  stesso tipo la condivide (warning), con un tipo diverso è un errore.
- **I parametri delle funzioni utente sono variabili globali statiche**: sopravvivono alla chiamata
  e sono visibili ovunque. Prova: `nextmapeffect`, parametro di `NextMap()`, è assegnato in
  `NextNMap()` e letto in `Tact()`, `NetTact()`, `MAPS.INI`, `PLAYER.INI`, `NET.INI` per decidere
  quale suono riprodurre dopo `Load()`. Un parametro con il nome di una globale dello stesso tipo
  ne condivide la memoria (warning). La ricorsione quindi non ha frame propri (il gioco non la usa).
- Gli argomenti sono valutati da sinistra a destra, poi assegnati ai parametri e convertiti al loro
  tipo. Gli argomenti in più sono valutati e ignorati (warning al link). Per quelli mancanti
  (`NextMap(demomap)` in `mainmenu.ini` e `win.ini`) vedi `MissingArguments`.

### Funzioni e valori di ritorno
- Le funzioni utente non hanno tipo di ritorno: `return(espr);` restituisce un valore,
  `return;` o la fine della funzione restituiscono `ExecConfig::defaultReturn` (0).
- `if(Tact()) return;` non ha nulla di speciale: `Tact()` è una funzione utente di `HEADER.INI`
  che restituisce 1 quando il livello deve sospendere la propria logica (menu aperto, fine
  partita, caricamento in corso) e 0 altrimenti.
- Una chiamata si lega prima a una funzione utente, poi a una builtin (`extern`), poi alle builtin
  predichiarate (`CompileOptions::predeclared`). `PlayMusic` è una funzione utente
  (`music.ini`) che chiama la builtin `PlayAudio` (97).
- `Load()` è una builtin come le altre: lo script continua (`Load(nextmap);if(…) PlaySFX(19);return;`
  in `MAPS.INI`); il cambio di script è compito dell'host, a tick finito.

### Builtin
- `extern Nome(parametri) ID;` dichiara una builtin. Il registro (`BuiltinRegistry`) permette la
  ricerca per ID e per nome; un nome può avere più dichiarazioni con arità diverse
  (`StopSFX()` 134 e `StopSFX(int nsfx)` 136) e la chiamata sceglie quella con lo stesso numero di
  argomenti; se esiste una sola dichiarazione e l'arità non coincide, la si usa (warning).
- `HEADER.INI` dichiara 76 builtin con ID 64–139, riportati in `src/script/builtin_ids.hpp`
  (nomi, ID e tipi dei parametri; `tests/script_data_test.cpp` verifica che coincidano).

### `iff`
Ogni `iff` del sorgente è un *sito* con un proprio stato. Con `IffMode::OncePerSite`
(predefinito) il corpo si esegue **la prima volta che la condizione è vera e mai più** (fino a
`resetIffs()` o a un nuovo `Interpreter`); lo stato si segna prima del corpo, così un `return`
interno non lo fa ripetere; dopo lo scatto la condizione non viene più valutata
(`iffEvaluateWhenFired` per valutarla comunque). È coerente con tutti gli usi del gioco:
- `mainmenu.ini`: `iff(1) {…;return;}` `iff(old_n_map!=27) {…;return;}` `iff(1) {…}`: con `main()`
  chiamato a ogni tick, il primo tick esegue il primo blocco, il secondo il filmato, il terzo
  l'inizializzazione: una sequenza.
- i livelli: `iff(1)` per l'inizializzazione, `iff(!GetObject(…)) Create(…)` per eventi una tantum.
- `Tact()`: `iff((GetTime()-nukestart)>2000 && win) {…}` per le fasi della schermata finale.

Un `iff` dentro un ciclo o una funzione resta un solo sito (in `levels/05.INI`
`for(i=1;i<3;i++) iff(…) { …Win(); }` scatta una volta sola in tutto).
Alternative selezionabili: `RisingEdge` (scatta a ogni passaggio falso→vero) e `PlainIf`.

### `main()` e i tick
`Interpreter::tick()` esegue `main()` una volta; la cadenza la decide l'host. Gli script misurano
il tempo con `GetTime()` (millisecondi), mai contando i tick, quindi il modello regge a 60 e 120 fps
purché `main()` giri a una frequenza ragionevole (si ipotizza una volta per tick logico).
Le globali si inizializzano alla costruzione dell'`Interpreter` (gli inizializzatori possono
chiamare builtin).

### Stato tra uno script e l'altro
Le variabili di `HEADER.INI` servono a passare informazioni tra script (commento originale accanto
a `old_n_map,n_map,map,n_player`): `n_map` impostato da `NextNMap()` nel menu è letto da `Tact()`
nel livello, `centr_x/centr_y` impostati in `mainmenu.ini` sono usati ovunque.
`Interpreter::snapshot()` e `inheritGlobals(snapshot, CarryOverPolicy)` copiano le globali con
lo stesso nome e tipo. Predefiniti (ipotesi):
- si copiano gli **interi**, **non le stringhe**: `nextmap` non viene mai azzerato prima di
  `Load(nextmap)`, e se sopravvivesse il nuovo script ricaricherebbe subito la mappa (Tact:
  `if(nextmap!="") … Load(nextmap)`);
- le variabili **con inizializzatore** riprendono il valore iniziale: `firstmenu=1` torna a 1 a
  ogni livello (`GameMenuOff()` lo azzera; il primo menu di un livello ha voci diverse dai
  successivi), mentre `kassandra`, `weapon`, `noammo` sono comunque ricaricati da `LoadPlayer()`.

## Modello di esecuzione configurabile

| Opzione | Predefinito | Alternative | Motivo |
|---|---|---|---|
| `ExecConfig::iffMode` | `OncePerSite` | `RisingEdge`, `PlainIf` | vedi `iff` |
| `ExecConfig::iffEvaluateWhenFired` | `false` | `true` | effetti collaterali della condizione |
| `ExecConfig::shortCircuit` | `true` | `false` | `&&`/`||` come in C; conta solo per builtin con effetti |
| `ExecConfig::missingArguments` | `KeepPrevious` | `Reset` | parametri statici: chi non è passato resta com'è |
| `ExecConfig::defaultReturn` | `0` | qualunque | `return;` e fine funzione |
| `ExecConfig::signedChars` | `false` | `true` | `s[i]` 0–255 o -128–127 (char di MSVC) |
| `ExecConfig::outOfRange` | `Warn` | `Error` | indici fuori dai limiti |
| `ExecConfig::maxSteps` | 10.000.000 | | protezione da cicli infiniti per tick/chiamata |
| `ExecConfig::maxCallDepth` | 200 | | protezione dalla ricorsione |
| `ExecConfig::traceUserFunctions` | `false` | `true` | `ScriptHost::userFunctionCalled` |
| `CompileOptions::implicitGlobals` | `false` | `true` | identificatori non dichiarati come `int` |
| `CompileOptions::allowFunctionRedefinition` | `false` | `true` | |
| `CompileOptions::strictUserArity` | `false` | `true` | argomenti in più: errore invece di warning |
| `CompileOptions::predeclared` | nessuno | `knownBuiltinRegistry()` | builtin senza `extern` (frammenti, test) |
| `CarryOverPolicy` | int sì, stringhe no, inizializzate no | | vedi sopra |

## Punti aperti (issue #3)

1. **`iff`**: una volta per sito (scelta attuale) o fronte di salita? Lo stato si azzera solo
   caricando un nuovo script? Un `iff` in un ciclo è davvero un solo sito?
2. **Cadenza di `main()`**: una volta per frame, per tick logico a frequenza fissa, o altro?
   `GetTime()` è il tempo reale o il tempo di gioco (in pausa si ferma)?
3. **Stato tra script**: quali globali sopravvivono a `Load()` (ipotesi: interi sì, stringhe no,
   inizializzatori rieseguiti) e se `Load()` interrompe lo script corrente.
4. **Argomenti mancanti** in una chiamata a funzione utente: restano invariati o si azzerano?
5. **Conversioni** stringa↔intero, `+` con stringhe, confronti misti; tipo restituito dalle
   builtin che restituiscono stringhe (`GetVidName`, `GetCommands`, `GetReg`, `FRead`,
   `GetNetInfo(NET_NAMEPLAYER)`), e cosa fa `PrintText` con un intero (`noammo[…]`).
6. **Valori di ritorno** di ogni builtin (per ora `0` negli stub) e significato di `Random(max)`
   (0..max-1 o 0..max: il commento originale dice "random 0-max").
7. **Accessi fuori dai limiti** e scritture oltre la fine di una stringa: l'originale usa memoria
   contigua? (rilevante solo per bug degli script).
8. Valutazione completa o in corto circuito di `&&`/`||`; ordine di valutazione degli argomenti.
9. `char` con o senza segno; divisione per zero; letterali ottali.
10. Sensibilità alle maiuscole degli identificatori e delle builtin (qui: sensibili).

## Strumento `script_run`

Compilato con `-DGROMADA_BUILD_TESTS=ON` (solo desktop): `build/tests/script_run`.

    script_run --ticks 3 --quiet GetTime maps/levels/05.INI        # dalla cartella del gioco
    script_run --root /percorso/gioco --set n_map=5 --trace --ticks 2 maps/levels/05.INI
    script_run --dump maps/MAPS.INI         # programma analizzato, con tutte le parentesi
    script_run --builtins maps/HEADER.INI   # registro delle builtin

Stampa ogni chiamata a builtin come `[tick] Nome(argomenti)` (stringhe tra virgolette con escape
C, byte non ASCII come `\xNN`), con `--trace` anche le funzioni utente (`> Nome(...)`), con
`--results` i valori restituiti, con `--globals` le variabili a fine esecuzione. Opzioni per il
modello: `--iff once|edge|if`, `--missing-args keep|reset`, `--no-short-circuit`, `--stub`
(tutte le builtin restituiscono 0 invece del mondo simulato).

## Test

- `tests/script_test.cpp`: frammenti sintetici scritti a mano (lexer, preprocessore, precedenze
  confrontate con il compilatore C++, parser, linker, tipi, stringhe, array, controllo di flusso,
  parametri statici, `iff` nei tre modelli, builtin, limiti, stato tra script, stampa dell'AST).
- `tests/script_data_test.cpp` (richiede `GROMADA_DATA`): compila tutti gli `.INI`, verifica che
  la stampa dell'AST sia stabile, confronta gli `extern` di `HEADER.INI` con `builtin_ids.hpp`,
  esegue 400 tick di ogni script con `main()` (anche entrando nei livelli dal menu) e controlla le
  chiamate dei primi tick di `levels/05.INI` (`SetVid`, `SetGamma`, `SetPosition`, `Create` del
  veicolo del giocatore dove punta la camera, `SetFlagman`, `PlayMusic` → `PlayAudio`).
- `script_run_demo`: lo strumento su uno script sintetico (`tests/data/script/`).
