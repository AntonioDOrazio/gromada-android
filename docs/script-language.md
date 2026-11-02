# Il linguaggio degli script di Gromada

Gli script `.INI` del gioco (menu, avvio missioni, condizioni di vittoria, eventi) sono scritti in
un piccolo dialetto del C. Il modulo `src/script/` è una **reimplementazione fedele** del
compilatore e della macchina virtuale di `gromada.exe` descritti in `docs/re/runtime.md` §1–2
(issue #9): il sorgente viene compilato nello **stesso bytecode** dell'originale e il bytecode
viene eseguito da una VM a pila che riproduce le istruzioni una per una, stranezze comprese.
Questo documento descrive le regole del linguaggio così come le applica il modulo, le poche
differenze inevitabili dall'originale e come è verificato.

## I file

- In `maps/` (CP866/CP1251/CP1252, righe CRLF):
  - **frammenti inclusi**: `HEADER.INI` (76 builtin, costanti, variabili condivise, funzioni comuni
    come `Tact()`, `Win()`, `Lose()`, i menu di gioco), `LOCALE.INI` (stringhe localizzate),
    `music.ini` (la funzione `PlayMusic`);
  - **39 script con `main()`**: `logo.ini`, `mainmenu.ini`, `PLAYER.INI`, `MAPS.INI`, `NET.INI`,
    `win.ini`, `credits.ini`, `levels/01.INI`…`levels/30.INI` (`18.ini` minuscolo),
    `net/05.INI`, `net/12.INI`.
- Ogni script con `main()` comincia con `#include "maps\header.ini"`: i percorsi sono DOS,
  relativi alla cartella del gioco, con maiuscole/minuscole libere. Per questo le prime 132 celle
  (le variabili dell'header) sono le stesse in tutti gli script.

## Struttura del modulo

| File | Contenuto |
|---|---|
| `source.hpp` | Caricamento dei sorgenti (`FileSystemSourceLoader`, ricerca senza distinzione di maiuscole; `MemorySourceLoader` per i test). |
| `compiler.hpp` | Il compilatore: porting del compilatore a discesa ricorsiva dell'originale (0x417cb0–0x41a790) via la replica Python `tools/re/runtime/gscript.py`; `Script` (risultato immutabile, condiviso). |
| `program.hpp` | Descrizione del programma compilato: celle (`CellDecl`), variabili (`GlobalVar`), funzioni (`FunctionDef`), `#define`, siti `iff` (`IffSite`), note del compilatore. |
| `bytecode.hpp` | Opcode, operandi, disassemblatore. |
| `interpreter.hpp` | La VM (`Interpreter`): celle, pila, istruzioni, patch delle `iff`, chiamate alle builtin. |
| `memory.hpp` | Spazio di indirizzi emulato per le stringhe (`StringMemory`). |
| `builtins.hpp`, `builtin_ids.hpp` | `ScriptHost`, `BuiltinInfo`, tabella delle 76 builtin dell'originale (ID, parametri, se restituiscono un valore). |
| `recording_host.hpp` | Host di prova che registra le chiamate; `SimulatedWorld` con risposte plausibili. |

## Uso del modulo

```cpp
#include "script/script.hpp"
using namespace gromada::script;

FileSystemSourceLoader loader{gameDir};
Script script = Script::compile(loader, "maps/levels/05.INI"); // come al caricamento della mappa
Interpreter vm{script, host};                // host: implementa ScriptHost::callBuiltin
vm.inheritGlobals(vmPrecedente.snapshot());  // cambio mappa: le celle sopravvivono (vedi sotto)
vm.setIntGlobalValues(demo.intGlobals);      // solo in riproduzione demo (src/demo/demo_file.hpp)
vm.tick();                                   // a ogni frame: esegue main() una volta
```

- Un caricamento di mappa = un nuovo `Script` e un nuovo `Interpreter` (il codice, con le sue
  patch delle `iff`, riparte dalla versione compilata). Le celle passano dal vecchio al nuovo con
  `inheritGlobals(vecchio.snapshot())`; senza, si parte come all'avvio del gioco (tutte a 0).
- `ScriptHost::callBuiltin(const BuiltinInfo&, std::span<const Value>)` riceve ogni chiamata a una
  builtin (opcode 64–139): il motore smista su `BuiltinInfo::id` (enum `BuiltinId`). Gli argomenti
  arrivano come sono (`int` o `string`); `argInt()`/`argString()` convertono come l'originale.
  Il valore restituito viene messo sulla pila solo se `ScriptHost::builtinPushesResult()` (di
  default la tabella dell'originale, `knownBuiltinPushes`) lo dice.
- `ScriptHost::warning()` riceve le anomalie non fatali (quelle che l'originale scrive in
  `error.log`). Gli errori fatali dell'originale (che chiudevano il gioco) sono `ScriptError`
  con fase `load`, `parse` (compilazione), `link` (manca `main()`) o `runtime`, e posizione
  `file:riga:colonna`.
- Accesso allo stato: `get/set(nome, indice)`, `cells()`, `cell(i)`, `cellValue(i)`,
  `intGlobalValues()`, `code()` (il codice con le patch), `iffFireCount(sito)`, `call(nome, args)`.

## Modello di esecuzione

- **Celle.** Ogni valore è una cella `{u32 valore, u8 flag}` (flag: `1` stringa, `2` int, `4`
  array, `8` inizializzata). Globali e pila stanno nello **stesso array di 2048 celle**: le
  variabili occupano le prime (`globalCount()` = numero di celle dichiarate), la pila parte subito
  dopo. Una cella stringa contiene un **indirizzo** (vedi «Stringhe»).
- **Frame.** `tick()` esegue il codice dall'offset 0, dove il compilatore ha messo
  `CALL main; JMP +30000`: `main()` gira da capo a ogni frame con `sp = bp = numero di globali`;
  il `JMP +30000` porta oltre la fine del codice (al massimo 20413 byte negli script del gioco) e
  termina il frame. Tra un frame e l'altro sopravvivono solo le celle e le patch delle `iff`. La
  cadenza (un frame per iterazione del ciclo principale, script fermo durante i filmati) è
  compito della sessione (`docs/architecture.md`).
- `tick()` restituisce il valore lasciato da `main()` nella cella della chiamata (vedi «Valori di
  ritorno»).

## Lessico e preprocessore (0x418000, 0x4180b0)

- Spazi: `isspace` (spazio, `\t \n \v \f \r`) e il byte 0. Commenti `// …` e `/* … */`; il `/*`
  termina al primo `*/` che segue (anche `/*/` è un commento chiuso).
- Identificatori: `[A-Za-z_0-9]`, al massimo 31 caratteri, **sensibili alle maiuscole**. In
  un'espressione un nome deve iniziare con una lettera (`int _x;` si dichiara ma non si usa).
- Numeri con `sscanf("%i")`: decimale, `0x` esadecimale, **`0` iniziale = ottale** (`017` = 15,
  `09` = 0); dopo il numero si saltano i caratteri `[0-9A-Fa-fx]`.
- Caratteri `'c'`: il byte **con segno** (`'\xE9'` = −23), **nessun escape** (`'\n'` è un errore).
- Stringhe `"…"`: `\n` e `\r`; ogni altro carattere preceduto da `\` vale sé stesso
  (`\\` = `\`, `\t` = `t`); **`\"` chiude la stringa**. Nessuna concatenazione di letterali.
- `#include "file"`: compila il file in quel punto; il nome è preso alla lettera (`maps\header.ini`).
- `#define NOME valore`: un intero (`%i`) o una stringa tra virgolette (senza escape); sostituito a
  compilazione. **Vince la prima definizione** (`V_M_FIGHTER` è definita due volte nell'header,
  con lo stesso valore). `#define` può comparire anche in mezzo a un'espressione. Ogni altra
  direttiva (`#undef`, `#if`…) è un errore.
- Ricerca dei nomi, in quest'ordine: funzioni (utente ed `extern`), `#define`, variabili, parole
  `int`/`string`/`extern`/`return`, poi definizione di funzione se segue `(`. Tutto **va dichiarato
  prima dell'uso** (compilatore a una passata).

## Grammatica

```
istruzione  := 'if' '(' espr ')' istruzione [ 'else' istruzione ]
             | 'iff' '(' espr ')' istruzione [ 'else' istruzione ]
             | 'while' '(' espr ')' istruzione
             | 'for' '(' espr ';' espr ';' espr ')' istruzione
             | 'break' ';'
             | '{' { istruzione } '}'         // '{}' va bene, '{ }' con spazi è un errore
             | '#include' "file" | '#define' NOME valore
             | espr ';'                        // anche vuota: ';' (usato in 'else;')
espr        := livello5
livello5    := livello4 { ('^' | '&&' | '&' | '||' | '|') livello4 }
livello4    := livello3 { ('>=' | '>>' | '>' | '<=' | '<<' | '<' | '==' | '!=') livello3 }
livello3    := livello2 { ('+' | '-') livello2 }
livello2    := primario { ('*' | '/' | '%') primario }
primario    := [ '-' | '~' | '!' ] [ '++' | '--' ] (
                 NUMERO | 'c' | "stringa" | '(' espr ')' | '#define' …
               | NOME [ '[' espr ']' ] [ '=' espr | '++' | '--' ]   // variabile
               | NOME '(' [ espr { car espr } ] ')'                 // chiamata
               | ('int'|'string') dichiaratori | 'extern' … | 'return' [ '(' espr ')' ]
               | NOME '(' parametri ')' '{' … '}' )                // definizione di funzione
dichiaratore := NOME [ '[' NUMERO ']' ] [ '=' NUMERO | '=' '{' NUMERO {',' NUMERO} '}' ]
```

- Tutti i livelli sono **associativi a sinistra**. Differenze dal C: confronti e shift hanno la
  **stessa precedenza** (`1<2<<3` = 8); `^ & && | ||` hanno la **stessa precedenza**
  (`1|2&0` = 0, `1||0&&0` = 0). Nessuno script del gioco dipende da queste differenze.
- Un solo operatore unario, applicato al primario (`-2*3` = −6, `!!x` è un errore).
  L'assegnazione fa parte del primario: `!unit=GetObject(…)` (`levels/01.INI`) è
  `!(unit=GetObject(…))`.
- Non esistono: `do`, `continue`, `switch`, `?:`, assegnazioni composte (`+=`), virgola, tipi
  diversi da `int`/`string`, tipo di ritorno delle funzioni.
- `return;` oppure `return(espr);`: **le parentesi sono obbligatorie** per restituire un valore.
- Nel `for` l'inizializzazione resta sulla pila fino alla condizione e l'incremento è compilato
  dopo il corpo. `break` esce dal ciclo più interno.
- Dopo `]` non si saltano spazi: `v[i]=x` va bene, **`v[i] = x` è un errore** (verificato nel
  disassemblato, 0x41879f). Tra gli argomenti di una chiamata qualunque carattere fa da separatore.
- `iff (…) … else …` è valido (vedi `iff`).

## Variabili

- **Tutte le variabili sono globali**: quelle in testa al file, quelle dichiarate dentro una
  funzione (la dichiarazione non genera codice) e i **parametri delle funzioni**, che sono
  dichiarazioni nell'elenco tra parentesi (`NextMap(string nextmapname;int nextmapeffect)`).
  Ridichiarare un nome è un errore, anche come parametro.
- Le celle sono assegnate in ordine di compilazione; un array `int v[3]` occupa tre celle
  consecutive con lo stesso nome (flag `4`); `string s[1]` non è un array. Un nome di array senza
  indice indica l'elemento 0 (`invisible=GetTime();`).
- **Inizializzatori** (`int a=5;`, `int v[3]={1,2};`): solo letterali numerici (`%i`, segno
  ammesso), applicati **una volta, alla compilazione**, cioè a ogni caricamento della mappa;
  nessun codice. Le stringhe non hanno inizializzatori (`string s="x";` è un errore).
- **Nessun controllo dei limiti**: `v[i]` con `i` fuori dall'array legge e scrive le celle delle
  variabili dichiarate dopo (o la pila). Fuori dalle 2048 celle: lettura 0, scrittura ignorata,
  `warning` (l'originale scriverebbe in memoria a caso).

### Cambio di mappa (`inheritGlobals`, `CarryOverPolicy::Mode::ByCell`)

Come nell'originale, l'array di celle **non viene azzerato** quando si carica un nuovo script:
1. le celle stringa dello script precedente vengono liberate e valgono 0 (0x417c30);
2. le dichiarazioni del nuovo script impostano i flag delle proprie celle, applicano gli
   inizializzatori e danno a ogni cella stringa un buffer nuovo e vuoto;
3. **tutto il resto conserva il valore per indice di cella** (non per nome): le int senza
   inizializzatore, gli elementi non coperti da un inizializzatore parziale
   (`int weapon[16]={88,0,0,0};` lascia `weapon[4..15]`, 19 casi negli script) e anche le celle
   oltre le globali del vecchio script, che contengono i resti della sua pila.

È il meccanismo con cui gli script si passano `n_map`, `old_n_map`, `map`, `n_player`, le opzioni:
l'header occupa sempre le stesse celle. Al primo avvio tutte le celle valgono 0 (nuovo
`Interpreter` senza `inheritGlobals`). La modalità `ByName` (vecchio comportamento ipotetico:
interi per nome, stringhe no, inizializzate no) resta per esperimenti.

## Tipi, conversioni, operatori

- `int`: 32 bit con segno, aritmetica modulare.
- Dove serve un int, una stringa è convertita con **`atoi`** (spazi, segno, cifre; altrimenti 0);
  dove serve una stringa, un int è scritto in decimale nel buffer statico di `_itoa`. Le
  conversioni sono silenziose.
- **Verità**: `if`, `while`, `for`, `!`, `&&`, `||` convertono a int: una stringa è vera se
  `atoi` ≠ 0 (`if("abc")` è falso, `if("12")` vero).
- `+`: **concatena solo se entrambi gli operandi sono stringhe** (nel buffer statico unico
  0x44498c); altrimenti somma intera (`"5"+1` = 6).
- `==` e `!=`: confronto con `strcmp` (byte senza segno) **se l'operando destro è una stringa**
  (l'altro, se int, è convertito in testo), altrimenti tra interi: `5=="5"` = 1, `"abc"==0` = 1,
  `0=="abc"` = 0. `!=` tra stringhe vale **−1/0/1** come `strcmp`, non 0/1.
- `< <= > >=` sono sempre tra interi (`"10">"9"` = 1). `&&` `||` `!` danno 0/1.
- **`&&` e `||` non fanno cortocircuito**: entrambi gli operandi sono sempre valutati.
- `/` tronca verso zero; **`x/0` = `0x0FFFFFFF`**. `x%0`, `INT_MIN/-1` e `INT_MIN%-1` fanno
  andare in crash l'originale (eccezione della CPU): qui `ScriptError` (predefinito) o, con
  `DivideErrorPolicy::Ignore`, 0 / `INT_MIN` / 0 con un `warning`.
- `<<` e `>>` usano `count & 31`; `>>` è aritmetico. `~` complemento, `-` negazione.
- `++`/`--` (prefissi e postfissi) agiscono solo su celle int: su una stringa non fanno nulla.
- **Ordine di valutazione**: strettamente da sinistra a destra, tranne **`a[i]=e`, dove `e` è
  valutato prima dell'indice**.
- L'assegnazione è un'espressione che vale il valore assegnato **con il tipo del lato destro**
  (`Message(n="12")` passa la stringa `"12"` anche se `n` è int); `a=b=0` funziona.

## Stringhe

- Ogni variabile stringa ha un buffer di 128 byte, vuoto a ogni caricamento; assegnare una
  stringa più lunga di 127 byte le dà un buffer della lunghezza giusta.
- Una cella stringa contiene un **indirizzo**: i valori sulla pila puntano al codice (letterali),
  ai buffer delle variabili, al buffer statico della concatenazione, a quello di `_itoa` o al
  buffer di una builtin. Per riprodurre gli effetti di aliasing dell'originale `memory.hpp` emula
  questi blocchi e copia i byte come le `rep movsd`/`rep movsb` inline dell'originale:
  - `"a"+"b" == "c"+"d"` vale **1** (le due concatenazioni usano lo stesso buffer);
  - `FRead(f) == FRead(f)` vale 1 (un solo buffer statico per `FRead`, `GetCommands`, `GetReg`);
    `GetVidName(1) == GetVidName(2)` invece confronta due testi distinti, come nell'originale.
- **`s[i] = v`** su una variabile stringa (non array) **scrive il byte `v` alla posizione `i` del
  buffer** (`nextmap[12]='0'+n_map/10;`); `v` è convertito con `atoi` (`s[1]="B"` scrive 0, che
  tronca). I byte oltre il terminatore restano quelli di prima (`"maps"` → `s[2]=0` → `"mA"` →
  `s[2]='x'` → `"mAxs"`).
- **Leggere `s[i]` non legge un carattere**: legge la cella `i` posizioni dopo `s` (nessuno script
  lo fa; il compilatore lo segnala con la nota `string-index-read`).
- Su una stringa array (`string t[3]`) l'indice sceglie l'elemento, come per gli int.

## Funzioni

- `Nome(parametri) { corpo }`, senza tipo di ritorno, definite **prima** dell'uso; ricorsione
  ammessa ma i parametri (globali) vengono sovrascritti.
- **Chiamata**: ogni argomento è valutato e **copiato nella cella del parametro** (`STORE p; EOS`),
  poi `CALL`. Argomenti mancanti: il parametro **mantiene il valore precedente**
  (`NextMap(demomap)`). Argomenti in più: scritti nelle celle dichiarate dopo i parametri.
- **Una chiamata a funzione utente distrugge i valori già sulla pila della stessa istruzione**
  (la `CALL` riusa la cella base del frame, l'`EOS` dopo ogni argomento azzera la pila):
  `x = F() + 1` funziona, `x = 1 + F()` è un errore fatale ("stack pop, but not push"). Gli
  script del gioco rispettano sempre la regola.
- **Valori di ritorno**: `return(e);` restituisce `e`. Con `return;` o alla fine del corpo
  l'originale restituisce la cella che conteneva l'indirizzo di ritorno: qui è riprodotto in modo
  deterministico, cioè il valore è **l'offset nel codice dell'istruzione dopo la `CALL`** (per
  `main()` vale 5), con i flag che quella cella aveva (di solito int; se l'ultimo argomento
  passato era una stringa la cella resta marcata stringa con un indirizzo non valido: l'originale
  andrebbe in crash leggendola, qui vale `""` con un `warning`). Gli script del gioco usano il
  valore solo di `Tact()` e `MoveInMenu()`, che terminano sempre con `return(…)`.
- `Interpreter::call(nome, args)` chiama una funzione come farebbe lo script.

## `iff`

`iff(cond) istr` si compila come un `if` con l'opcode IFF. La prima volta che `cond` è vera,
IFF **riscrive il codice**: mette un `JMP` al bersaglio falso dell'`iff` all'**inizio
dell'istruzione corrente** e poi esegue `istr`. L'«inizio dell'istruzione corrente» non è
calcolato in compilazione: è la posizione dopo l'ultima `EOS` (`;`), `JZ` (`if`/`while`/`for`),
`IFF`, `CALL` o `RET` eseguita. Il modulo fa esattamente questa patch (`IffMode::Patch`).
Conseguenze:
- dopo lo scatto la condizione non viene più valutata (nessun effetto collaterale);
- la patch dura fino al prossimo caricamento della mappa (nuovo `Interpreter`, o `resetIffs()`);
- è per **punto del codice**: una `iff` in un ciclo o in una funzione chiamata da più punti scatta
  una volta sola in tutto;
- una serie di `iff(1) {…; return;}` (`mainmenu.ini`) esegue un blocco per frame: ogni scatto
  riscrive l'inizio dell'istruzione corrente in modo da saltare al blocco successivo (se le `iff`
  sono le prime istruzioni di `main()` è l'inizio di `main()` stesso, fissato dalla `CALL`);
- se l'`iff` è raggiunta con un **salto** (fine del ramo `if` di un `if/else`, `break`), è quel
  `JMP` a essere riscritto: l'`iff` non verrà più raggiunta da quella strada ma **può scattare
  una seconda volta** se raggiunta da un'altra. Succede in 7 punti degli script:
  `HEADER.INI:751` (in `Tact()`), `levels/01.INI:128`, `12.INI:114`, `15.INI:87`, `18.ini:125`,
  `19.INI:132`, `27.INI:82` (`IffSite::reachableByJump`; `runtime.md` §2.9 ne contava 6 perché il
  controllo non leggeva `18.ini`);
- `iff(c) A; else B;`: finché `c` è falsa si esegue `B`; dopo lo scatto si esegue sempre `B`.

Alternative per esperimenti: `IffMode::OncePerSite` (patch sulla condizione stessa, nota in
compilazione: esattamente una volta per sito; differisce solo nei 7 punti sopra) e
`IffMode::PlainIf` (nessuna patch).

## Builtin

- `extern Nome(testo qualunque) ID;` dichiara una builtin: il testo tra parentesi è ignorato
  (documentazione), l'ID è letto con `%i` e **troncato a un byte** e diventa l'opcode della
  chiamata. **Vince la prima dichiarazione** di un nome: `StopSFX` è dichiarata 134 e 136, ogni
  chiamata usa 134 (la 136 è irraggiungibile). Un ID sotto 64 è un'istruzione della VM
  (`extern Plus() 3;` fa un `ADD`).
- Gli opcode 64–139 hanno un handler: l'interprete estrae dalla pila **il numero di argomenti
  dell'handler** (`knownBuiltinArity`, da `kKnownBuiltins`, non dalla dichiarazione), li passa
  all'host nell'ordine in cui sono stati scritti e mette sulla pila il risultato solo per le
  builtin che restituiscono qualcosa (`knownBuiltinPushes`, colonna «Ritorno» di `runtime.md` §3;
  `GetNetInfo` solo per i tipi 1–6 e 10–25). Usare in un'espressione una builtin che non
  restituisce nulla è un errore fatale (pila vuota); argomenti in più restano sulla pila e
  producono uno «stack error» non fatale a fine istruzione; argomenti in meno sono un errore
  fatale.
- Gli altri opcode (0, 26, 27, 39–63, 140–255) sono «Unknown command»: `warning` e fine del frame.
- `HEADER.INI` dichiara 76 builtin con ID 64–139, in `builtin_ids.hpp` (nomi, ID, parametri;
  `tests/script_data_test.cpp` verifica che coincidano). Semantica di ciascuna: `runtime.md` §3.

## Errori

- **Fatali** nell'originale (chiudevano il gioco), qui `ScriptError`: errori di compilazione (con
  il messaggio originale, es. `LOGIC::Unknown name x`), `main()` mancante, «stack pop, but not
  push», «stack overflow» (più di 2048 celle, anche per ricorsione infinita), divisioni non
  rappresentabili (vedi sopra), troppe istruzioni in un frame (`ExecConfig::maxSteps`, protezione
  dai cicli infiniti che nell'originale bloccherebbero il gioco).
- **Non fatali** (l'originale li scrive in `error.log`), qui `ScriptHost::warning`: pila non vuota
  a fine istruzione («`;`/if/iff::stack error»), «Unknown command», accessi a memoria inesistente.

## Opzioni

| Opzione | Predefinito | Alternative |
|---|---|---|
| `ExecConfig::iffMode` | `Patch` (l'originale) | `OncePerSite`, `PlainIf` |
| `ExecConfig::divideError` | `Throw` | `Ignore` |
| `ExecConfig::outOfRange` | `Warn` | `Error` |
| `ExecConfig::maxSteps` | 10.000.000 istruzioni per frame | |
| `ExecConfig::traceUserFunctions` | `false` | `true`: `ScriptHost::userFunctionCalled` |
| `CompileOptions::requireMain` | `true` (l'originale) | `false` per compilare i frammenti |
| `CompileOptions::predeclared` | nessuno | `knownBuiltinRegistry()`: builtin senza `extern` |
| `CarryOverPolicy::mode` | `ByCell` (l'originale) | `ByName` |

## Differenze inevitabili dall'originale

Riguardano solo comportamenti che nell'originale dipendono da indirizzi di memoria o che lo
farebbero andare in crash; nessuno script del gioco li attraversa (il controllo statico e i test
lo verificano):
- gli indirizzi delle stringhe sono deterministici ma diversi da quelli dell'heap originale
  (conta solo se uno script usasse un indirizzo come numero, per esempio con `s[i]` in lettura);
- puntatori non validi, scritture oltre i buffer, celle fuori dalle 2048: ignorati con `warning`
  (l'originale corromperebbe la memoria o andrebbe in crash);
- una stringa più lunga di 127 byte riceve il nuovo buffer prima che il vecchio sia liberato
  (l'originale libera prima: con `s=s` leggerebbe memoria liberata);
- la concatenazione oltre i 1024 byte del buffer statico è troncata (l'originale sfora);
- le conversioni int → stringa degli argomenti delle builtin le fa l'host (`argString`): se una
  builtin con due parametri stringa riceve due int, l'originale li convertirebbe entrambi nello
  stesso buffer statico di `_itoa` e vedrebbe due volte il primo argomento;
- i risultati stringa delle builtin diverse da `FRead`/`GetCommands`/`GetReg` sono conservati in
  buffer distinti per contenuto (nell'originale puntano ai dati del motore).

## Verifica

- `tests/script_test.cpp`: frammenti sintetici scritti a mano, uno o più per ogni regola di questo
  documento (bytecode emesso, lessico, dichiarazioni, precedenze, ordine di valutazione, stringhe e
  aliasing, array, funzioni e valori di ritorno, `iff` compreso il doppio scatto dopo un salto,
  builtin, limiti, cambio di mappa e istantanee delle demo).
- `tests/script_data_test.cpp` (richiede `GROMADA_DATA`): i 39 script compilano in un bytecode e in
  una disposizione delle celle identici alla replica `gscript.py` (dimensioni, conteggi e hash
  FNV-1a ricavati da `gscript.py`); i frammenti compilano; gli `extern` dell'header coincidono con
  `kKnownBuiltins`; le `iff` raggiunte da un salto sono le 7 note; per le **3 demo** il numero di
  celle int coincide con quello registrato, ogni cella inizializzata vale il suo inizializzatore
  nella registrazione e, caricata l'istantanea, `n_map` è il numero del livello e
  `centr_x`/`centr_y` valgono 320/240; ogni script gira 400 frame senza errori né warning;
  il passaggio `MAPS.INI` → `levels/05.INI` conserva le celle per indice; il primo frame di
  `levels/05.INI` fa le chiamate attese.
- `tests/script_layout_compare.py` (test `script_layout_compare`, richiede `python3` e
  `GROMADA_DATA`): confronta byte per byte il bytecode, le celle, le funzioni, gli `extern`, i
  `#define` e i siti `iff` di `script_run --layout` con `gscript.py` su tutti i 39 script.

## Strumento `script_run`

Compilato con `-DGROMADA_BUILD_TESTS=ON` (solo desktop): `build/tests/script_run`.

    script_run --ticks 3 --quiet GetTime maps/levels/05.INI        # dalla cartella del gioco
    script_run --root /percorso/gioco --set n_map=5 --trace --ticks 2 maps/levels/05.INI
    script_run --dis maps/MAPS.INI          # bytecode disassemblato
    script_run --layout maps/MAPS.INI       # bytecode, celle, funzioni (per il confronto)
    script_run --notes maps/MAPS.INI        # note del compilatore (stranezze incontrate)
    script_run --fragment --builtins maps/HEADER.INI
    script_run --fragment --predeclare --dis maps/music.ini

Stampa ogni chiamata a builtin come `[frame] Nome(argomenti)`, con `--trace` anche le funzioni
utente (`> Nome(...)`), con `--results` i valori restituiti, con `--globals` le variabili a fine
esecuzione. `--iff patch|site|if`, `--stub` (tutte le builtin restituiscono 0).

## Cosa è cambiato rispetto alla prima versione (issue #1 → #9)

La prima versione era un interprete ad albero di un C «ragionevole», scritto prima del reverse
engineering. Ora:

| Regola | Prima | Ora (come l'originale) |
|---|---|---|
| Esecuzione | AST interpretato | bytecode identico all'originale su una VM a pila |
| `&&`, `\|\|` | cortocircuito | entrambi gli operandi sempre valutati |
| Precedenze | quelle del C | confronti = shift; `^ & && \| \|\|` allo stesso livello |
| `x/0`, `x%0` | 0 | `0x0FFFFFFF`; `%0` errore (crash nell'originale) |
| `a[i]=e` | indice poi valore | valore poi indice |
| `iff` | flag per sito | patch del codice all'inizio dell'istruzione corrente (doppio scatto possibile) |
| Inizializzatori | espressioni eseguite alla creazione dell'interprete | letterali, alla compilazione |
| Cambio mappa | per nome, int sì, inizializzate e stringhe no | per indice di cella, resti della pila compresi |
| Verità di una stringa | non vuota | `atoi` ≠ 0 |
| `==` misto | tra interi | `strcmp` se l'operando destro è stringa |
| `s[i]` in lettura | carattere | cella successiva |
| `s[i]=v` | primo byte di `v`, accoda/tronca | byte `atoi(v)` scritto nel buffer |
| `return;` | 0 | offset di ritorno (deterministico) |
| `#define`/`extern` doppi | vince l'ultimo / scelta per arità | vince il primo |
| Dichiarazioni nelle funzioni, `return x;`, `do`, `?:`, `+=`, `continue` | accettati o errori diversi | come l'originale |
| Uso prima della dichiarazione | ammesso | errore |
| Builtin | tutti gli argomenti, risultato sempre usabile | argomenti e risultato secondo l'handler originale |
