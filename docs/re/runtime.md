# Runtime di gromada.exe: script, builtin, menu, input, salvataggi, demo (issue #3)

Analisi di `gromada.exe` (PE32, MSVC, dicembre 1999, image base `0x400000`) per la parte che
esegue gli script `.INI` e gestisce interfaccia, input, salvataggi, demo, audio, video e testi.
Gli indirizzi sono VA dell'eseguibile originale. Gli strumenti usati sono in
`tools/re/runtime/` (vedi in fondo). Ghidra non era scaricabile da questo ambiente: si è usato
angr (decompilatore) + capstone, verificando sul disassemblato ogni punto riportato come certo.

Livelli di confidenza:
- **alta**: letto nel disassemblato e, dove possibile, verificato sui dati del gioco
  (per esempio il compilatore replicato produce esattamente il numero di variabili che le demo
  registrano, e il parser delle demo termina esattamente sul marcatore di fine);
- **media**: dedotto dal decompilato/disassemblato ma non verificato sui dati;
- **bassa**: ipotesi plausibile.

---

## 1. Per l'interprete (riepilogo per la issue #1)

Regole di esecuzione da seguire. Tutte a confidenza **alta** salvo dove indicato.

### 1.1 Modello generale
- Gli script **non sono interpretati come testo**: al caricamento della mappa il file
  `<mappa>.ini` viene **compilato in un bytecode per una macchina a pila** (compilatore a discesa
  ricorsiva, 0x417cb0–0x41a790) ed eseguito dalla VM `0x41a790`. Un modo fedele e semplice per la
  issue #1 è replicare lo stesso bytecode: `tools/re/runtime/gscript.py` è una replica del
  compilatore (vedi §2) e compila senza errori tutti i 39 script del gioco. Non esiste bytecode
  originale con cui confrontarla byte per byte, ma la disposizione delle variabili coincide con
  quella registrata nelle tre demo (§8).
- **`main()` viene eseguita da capo una volta per frame** di gioco (una "tick"): la VM parte
  sempre dall'offset 0, dove il compilatore ha messo `CALL main; JMP +30000`. Non c'è stato di
  esecuzione che sopravviva tra un frame e l'altro, tranne le **variabili globali** e le
  **`iff` già scattate**. Un `return` in `main` termina l'esecuzione di quel frame.
- Ordine dentro un frame (0x401550 → 0x4141f0): avanzamento orologio (`GetTime`), campionamento
  input, **script (`main`)**, simulazione (0x40b880), aggiornamento menu (0x40c1d0: hover/click),
  input del giocatore (0x415e20: scorrimento, cursore, comandi di selezione), azzeramento di
  tasto e click, rendering. Quindi `MenuVid()` letto dallo script al frame N riflette il click
  elaborato alla fine del frame N−1, e vale per un solo frame.
- Il frame rate non è fisso: un frame per iterazione del ciclo dei messaggi. L'orologio di gioco
  avanza del tempo reale trascorso, **limitato a 71 ms per frame** (0x401f70).
- Lo script non gira mentre un video AVI è in riproduzione (il ciclo principale non chiama la tick).

### 1.2 Tipi e celle
- Ogni valore è una "cella" di 5 byte: 4 byte di valore + 1 byte di flag. Pila e variabili
  globali stanno nello **stesso array** di 2048 celle (`LOGIC+0x11c`): le globali occupano le
  prime celle, la pila di valutazione parte subito dopo.
- Flag: `1` = stringa (il valore è un `char*`), `2` = int, `4` = array, `8` = inizializzata.
- Conversioni implicite: dove serve un int, una stringa è convertita con `atoi`; dove serve una
  stringa, un int è convertito in decimale (`_itoa(v, buf, 10)`) in **un unico buffer statico**
  (0x44e590), quindi due conversioni nella stessa istruzione si sovrascrivono.
- Le variabili stringa hanno ciascuna un buffer di 128 byte (riallocato se si assegna una
  stringa più lunga di 127 caratteri). Inizializzate a `""` a ogni caricamento.
- Interi a 32 bit con segno, aritmetica in complemento a 2 senza controlli di overflow.

### 1.3 Variabili, persistenza, inizializzatori
- **Tutte le variabili sono globali**, anche quelle dichiarate dentro una funzione e anche i
  **parametri delle funzioni** (sono variabili globali dichiarate nell'elenco dei parametri).
  `int`/`string` dentro un corpo di funzione dichiara una globale a tempo di compilazione e non
  genera codice.
- Gli **inizializzatori** (`int a=5;`, `int v[3]={1,2,3};`) sono applicati **una volta, al
  momento della compilazione** (cioè al caricamento della mappa), non a runtime. Accettano solo
  letterali numerici (`sscanf("%i")`: decimale, `0x` esadecimale, **`0` iniziale = ottale**,
  segno ammesso). Le stringhe non hanno inizializzatori.
- **Le globali int senza inizializzatore mantengono il valore tra un caricamento di mappa e
  l'altro**: le celle non vengono azzerate, e poiché ogni script inizia con
  `#include "maps\header.ini"` le globali dell'header occupano sempre le stesse celle. È il
  meccanismo con cui gli script si passano informazioni (`n_map`, `old_n_map`, `map`,
  `n_player`, opzioni...). Vale anche per gli elementi di array **non coperti** da un
  inizializzatore parziale (`int weapon[16]={88,0,0,0};` lascia invariati `weapon[4..15]`):
  19 casi negli script, `levels/18.ini` compreso. Al primo avvio tutte le celle valgono 0. Le stringhe invece ripartono da
  `""` a ogni caricamento.
- Una variabile array usata senza indice indica il suo primo elemento (`invisible=GetTime();`,
  `FWrite(win,nweapon)`).
- Nessun controllo dei limiti negli indici: un indice fuori dall'array legge/scrive le celle
  delle variabili dichiarate dopo.
- Limiti: 256 celle di variabili (gli script del gioco arrivano a 243), 128 funzioni (extern
  incluse), 256 `#define`, nomi lunghi al massimo 31 caratteri.

### 1.4 Stringhe e indicizzazione
- `s[i] = v;` su una variabile **stringa** non-array scrive **il byte** `v` in posizione `i` del
  buffer (`nextmap[12]='0'+n_map/10;`). È l'unico uso dell'indice sulle stringhe.
- Leggere `s[i]` su una stringa **non** restituisce un carattere: legge la cella della
  variabile che sta `i` posizioni dopo (nessuno script lo fa).
- `+` tra due stringhe concatena (risultato in un buffer statico unico, 0x44498c); se solo uno
  dei due operandi è stringa, si fa la somma intera (`atoi`).
- `==` e `!=` confrontano con `strcmp` se il **secondo** operando (quello in cima alla pila) è
  una stringa, altrimenti come interi.
- Letterali: `"..."` con escape `\n`, `\r` e `\x` → `x` (ma `\"` non funziona: chiude la
  stringa); `'c'` = codice del carattere (con segno, nessun escape).

### 1.5 Operatori, precedenza, ordine di valutazione
Livelli dal più stretto al più largo (tutti associativi a sinistra):
1. primario: letterale, variabile (con `[indice]`, `=`, `++`/`--` prefissi e postfissi),
   chiamata, `( expr )`; operatori unari `-`, `~`, `!` (applicati al primario intero);
2. `*` `/` `%`;
3. `+` `-`;
4. `>` `>=` `<` `<=` `==` `!=` `<<` `>>` **tutti allo stesso livello**;
5. `^` `&` `&&` `|` `||` **tutti allo stesso livello**.

Differenze da C: confronti e shift hanno la stessa precedenza; `& ^ | && ||` hanno la stessa
precedenza; **`&&` e `||` non cortocircuitano** (entrambi gli operandi vengono sempre
valutati, risultato 0/1). Il controllo statico (§2.9) mostra che nessuno script del gioco
mescola operatori il cui risultato cambierebbe rispetto a C, ma la mancanza di cortocircuito
conta per gli effetti collaterali (chiamate a builtin in entrambi i lati).

Altro:
- valutazione strettamente da sinistra a destra; gli argomenti delle builtin sono valutati da
  sinistra a destra e restano sulla pila;
- in `a[i] = expr` viene valutato **prima `expr` e poi l'indice**;
- l'assegnazione è un'espressione: vale il valore assegnato **con il tipo del lato destro**
  (`PlayMusic(music1=8, ...)` passa 8); `a=b=0` funziona;
- `++`/`--` agiscono solo su celle int (su una stringa non fanno nulla);
- `/` è divisione intera troncata (`idiv`); **`x/0` vale `0x0FFFFFFF`**; `x%0` manda in crash
  l'originale; `>>` è aritmetico; gli shift usano `count & 31`;
- `!x` vale 1 se `x==0`, altrimenti 0; confronti con segno.

### 1.6 Funzioni, parametri, valori di ritorno
- Definizione: `Nome(dichiarazioni dei parametri) { corpo }`, senza tipo di ritorno. I parametri
  sono dichiarazioni separate da **`;`** (`NextMap(string nextmapname;int nextmapeffect)`).
- Le funzioni vanno definite **prima** dell'uso (la ricerca dei nomi avviene in compilazione);
  ammessa la ricorsione su se stesse ma i parametri, essendo globali, vengono sovrascritti.
- Chiamata di una funzione utente: ogni argomento è valutato e **copiato nella globale del
  parametro** (`STORE param; EOS`), poi `CALL`. Argomenti mancanti: il parametro mantiene il
  valore precedente (`NextMap(demomap)` in `mainmenu.ini` lascia `nextmapeffect` com'era).
  Argomenti in più: scritti nelle variabili dichiarate dopo i parametri.
- `return;` e `return(expr);` (le parentesi sono obbligatorie per restituire un valore).
- Valore restituito: il valore in cima alla pila della funzione al momento del `return`; con
  `return;` o arrivando alla fine del corpo il valore è **indefinito** (l'originale restituisce
  la cella che conteneva l'indirizzo di ritorno). Negli script del gioco si usa il valore solo di
  `Tact()` e `MoveInMenu()`, che terminano sempre con `return(...)`.
- **Una chiamata a funzione utente distrugge i valori già sulla pila della stessa istruzione**
  (la `CALL` riusa la cella base del frame). Una chiamata utente è sicura solo come primo valore
  valutato di un'istruzione o condizione (`x = F();`, `if(Tact()) return;`, `if(!MoveInMenu(...))`).
  Il controllo statico conferma che gli script rispettano sempre questa regola.
- `main()` è obbligatoria (errore fatale "Not enough main() function").

### 1.7 `iff` (semantica esatta)
`iff(cond) stmt` compila come `if`, ma con un'istruzione IFF (opcode 29) che **la prima volta
che `cond` è vera esegue `stmt` e poi si auto-disattiva modificando il bytecode**: sostituisce
l'inizio dell'istruzione con un salto incondizionato oltre `stmt`. Conseguenze:
- dopo essere scattata, **la condizione non viene più valutata** (nessun effetto collaterale);
- la disattivazione dura fino al prossimo caricamento della mappa (il bytecode viene
  ricompilato); è per **sito nel codice**, non per chiamata: una `iff` in una funzione chiamata da
  più punti scatta una volta sola in totale, una `iff` in un ciclo scatta una volta sola;
- `iff(1) {...}` = "esegui una volta all'avvio dello script".

Dettaglio fine (da replicare per fedeltà totale): il punto che viene sovrascritto non è
calcolato in compilazione ma è "l'inizio dell'istruzione corrente" tenuto dalla VM, cioè la
posizione dopo l'ultimo `;` (EOS), `if`/`while`/`for` (JZ), IFF, CALL o RET eseguiti. Se la `iff`
è raggiunta con un salto (fine del ramo `if` di un `if/else`, `break`), quel punto è il salto
stesso: è il salto a essere trasformato e la `iff` può scattare **una seconda volta** se poi viene
raggiunta per un'altra strada. Succede in 7 punti degli script (§2.9), tra cui
`iff(n_map>0 && n_map<=30)` in `Tact()` quando nello stesso frame scatta un ramo della catena
dei cheat (evento praticamente impossibile al primo frame). Un'implementazione con "flag per
sito" si discosta dall'originale solo in quei 7 punti e solo in sequenze rare (negli script la
seconda esecuzione è innocua); per la fedeltà assoluta conviene il bytecode.
`iff ... else` è sintatticamente valido (dopo lo scatto si esegue sempre il ramo `else`) ma nessuno
script lo usa.

### 1.8 `Tact()`
`Tact()` **non è una builtin**: è una funzione script definita in `maps/HEADER.INI` (riga ~726) che
ogni livello chiama con `if(Tact()) return;`. Gestisce cheat, caricamento del giocatore
(`LoadPlayer()` nella prima `iff`), menu di gioco/opzioni/design, sconfitta/vittoria, cambio
mappa, armi e oggetti raccolti. Restituisce 1 quando il resto di `main` non deve girare (menu
aperto, cambio mappa in corso, effetto vittoria/sconfitta) e 0 altrimenti.
`NetTact()` è l'equivalente per la rete. `act_calltact` (49) è un'azione delle unità, non
c'entra con gli script.

### 1.9 Builtin
- `extern Nome(param) ID;` registra la builtin con opcode `ID` (un byte). Gli argomenti restano
  sulla pila e l'handler li estrae in ordine inverso; le builtin che restituiscono qualcosa
  spingono **un** valore (int o stringa), le altre **nessuno** (usarle in un'espressione causa un
  errore di pila). La tabella completa è al §3, con la colonna "ritorno".
- Nomi duplicati: vince la **prima** dichiarazione. `StopSFX` è dichiarata due volte (134 e 136):
  ogni chiamata usa la 134, che non prende argomenti.
- Gli handle delle unità (`Create`, `Flagman`, `GetObject`, `MenuCreate`, `PrintText`...) sono
  **puntatori C++** dell'originale: per lo script sono interi opachi, 0 = nessuno. I file
  (`FOpen`/`FCreate`) sono `FILE*`, 0 = errore.

### 1.10 Preprocessore e lessico
- `#include "file"` compila il file in quel punto (percorso relativo alla cartella del gioco,
  `\` letterale, nessun escape); `#define NOME valore` con valore intero (`%i`) o stringa
  `"..."` (senza escape); la costante viene sostituita a compilazione. Nessun'altra direttiva.
  Un valore negativo o un'espressione dopo `#define` non viene letto per intero: il resto finisce
  nel codice (nessuno script lo fa).
- Commenti `//` e `/* */`; spazi e byte 0 ignorati. Identificatori `[A-Za-z][A-Za-z0-9_]*`,
  **case sensitive** (i percorsi dei file invece no, perché su Windows).
- Ricerca dei nomi: prima funzioni (utente ed extern), poi `#define`, poi variabili, poi parole
  chiave `int`, `string`, `extern`, `return`, poi definizione di funzione se segue `(`.
- Parole chiave delle istruzioni: `if`, `iff`, `else`, `while`, `for`, `break`. Non esistono
  `do`, `switch`, `continue`, `?:`, tipi diversi da `int`/`string`.
- Blocco vuoto: `{}` va bene, `{ }` con spazi dentro è un errore di compilazione nell'originale.
  Istruzione vuota: `;` (usata in `else;`).
- Dopo `for(init;cond;incr)`: `init` è valutato senza `;` finale (il valore resta sulla pila fino
  alla condizione); `incr` è compilato dopo il corpo. `break` esce dal ciclo più interno.

### 1.11 Errori dell'originale
- Fatali (scrivono `error.log` ed escono): errori di compilazione, "stack pop, but not push",
  "stack overflow" (più di 2048 celle), memoria esaurita.
- Non fatali (solo `error.log`): "if/iff/';'::stack error" (pila non vuota a fine istruzione: la
  pila viene comunque azzerata), "Unknown command" (opcode senza handler: termina il frame).

---

## 2. Compilatore e VM in dettaglio

### 2.1 Oggetti e caricamento
- Oggetto globale del mondo/partita: `0x439d68` ("obj"). L'oggetto LOGIC (script) è a
  `obj+0x9a9` = `0x43a711`. Campi LOGIC: `+0x000` nome file (256), `+0x100` codice, `+0x104`
  lunghezza codice, `+0x108` puntatore di lettura del sorgente, `+0x10c` fine sorgente, `+0x110`
  buffer sorgente, `+0x114` sp, `+0x118` bp (base del frame), `+0x11c` celle (2048×5 byte),
  `+0x291c` numero di globali, `+0x2920` numero di funzioni, `+0x2924` numero di define.
  Tabelle statiche: funzioni a `0x44d190` (40 byte: nome, `+0x21` opcode, `+0x22` indirizzo,
  `+0x26` indice del primo parametro), nomi delle variabili a `0x440988` (32 byte), nomi dei
  define a `0x44298c` (32 byte), valori stringa a `0x444d8c` (128 byte), valori int a `0x44cd8c`.
- `Load(mappa)` → 0x4149b0: scarica la mappa precedente, carica la `.map`, poi
  `0x417ea0(<mappa senza estensione>.ini)`. Se il `.ini` manca, lo script è vuoto (solo log).
- 0x417ea0 legge i primi 4 byte: se il byte alto è ≠ 0 il file è **sorgente** e va compilato
  (0x417cb0); altrimenti è uno **script precompilato**: `u32 ncelle`, per cella `u8 flag`
  (+`u32 valore` se `flag&8`), `u32 lunghezza codice`, codice. Il gioco distribuisce solo sorgenti.
  (confidenza alta sul codice, il formato precompilato non è usato dai dati)
- 0x417cb0: libera le stringhe del precedente script (0x417c30), alloca 64000 byte di codice,
  scrive `1E 00000000 1C 3075` (CALL main; JMP +30000), azzera i contatori e compila istruzione
  per istruzione fino alla fine del file. Le istruzioni a livello superiore (dichiarazioni,
  definizioni) generano codice morto che non viene mai eseguito. Il codice compilato degli script
  del gioco va da 14966 a 20413 byte, quindi il `JMP +30000` porta sempre oltre la fine (= stop).

### 2.2 Lessico (0x418000, 0x4180b0)
Come al §1.10. `isspace`/`isalpha`/`isalnum`/`isdigit`/`isxdigit` della CRT MSVC (0x41c720,
0x41c69d, 0x41c748, 0x41c6cb, 0x41c6f3). Un numero è letto con `sscanf("%i")` e poi si saltano i
caratteri `[0-9A-Fa-fx]`.

### 2.3 Grammatica ed emissione (0x418140 primario, 0x4196d0 `* / %`, 0x4197d0 `+ -`,
0x419880 confronti/shift, 0x419b40 `^ & && | ||`, 0x419cf0 istruzioni)

Nella tabella gli opcode sono in esadecimale (nel §2.4 in decimale).

| Costrutto | Bytecode emesso |
|---|---|
| numero, `'c'`, define int | `01 imm32` |
| `"..."`, define stringa | `02 bytes 00` |
| variabile `v` | `17 v` (PUSHV) |
| `v[i]` | `<i> 24 17 v` |
| `v = e` / `v[i] = e` | `<e> 18 v` / `<e> <i> 24 18 v` (STORE) |
| `v++` `v--` `++v` `--v` | `20 v` `21 v` `22 v` `23 v` |
| `op e` unario | `<e> 08` (−), `09` (~), `13` (!) |
| `a op b` | `<a> <b> op` |
| builtin `f(a,b)` | `<a> <b> ID` |
| utente `f(a,b)` | `<a> 18 p0 19 <b> 18 p1 19 1E addr32` |
| `e;` | `<e> 19` (EOS) |
| `if(c) s` | `<c> 16 rel16 <s>` |
| `if(c) s else t` | `<c> 16 rel16 <s> 1C rel16 <t>` |
| `iff(c) s` | `<c> 1D rel16 <s>` |
| `while(c) s` | `L: <c> 16 rel16 <s> 1C →L` |
| `for(i;c;n) s` | `<i> L: <c> 16 rel16 <s> <n> 19 1C →L` |
| `break;` | `1C rel16` (corretto alla fine del ciclo) |
| `return;` / `return(e);` | `1F` / `<e> 1F` |
| funzione | corpo + `1F`, poi un `19` morto (il `}` finale viene sovrascritto con `;`) |

Gli offset relativi sono `int16` riferiti alla posizione dell'operando stesso
(destinazione = posizione dell'operando + rel). Variabili e parametri sono indicizzati con **un
byte** (indice della cella globale).

### 2.4 Opcode della VM (0x41a790, tabella di salto a 0x41b164)

pop/push: `pop_int` 0x41b330 (stringa → `atoi`), `pop_str` 0x41b200 (int → `_itoa` in 0x44e590),
`push_int` 0x41b2d0, `push_str` 0x41b270 (spinge il puntatore, senza copia).

| Op | Nome | Semantica |
|---|---|---|
| 1 | PUSHI imm32 | spinge int |
| 2 | PUSHS str | spinge puntatore alla stringa nel codice |
| 3 | ADD | se le due celle in cima sono stringhe: concatena in 0x44498c; altrimenti somma int |
| 4–7 | SUB MUL DIV MOD | `a-b`, `a*b`, `a/b` (b=0 → 0x0FFFFFFF), `a%b` |
| 8, 9 | NEG, NOT | `-x`, `~x` |
| 10, 11, 12 | XOR OR AND | bit a bit |
| 13, 20 | EQ, NE | `strcmp` se la cima è stringa, altrimenti int |
| 14, 21 | LOR, LAND | 0/1, entrambi gli operandi già valutati |
| 15–18 | GT LT GE LE | con segno |
| 19 | LNOT | `x==0` |
| 22 | JZ rel | pop; se 0 salta; inizio istruzione := nuovo pc; sp := bp |
| 23 | PUSHV v | copia la cella `v+indice` (valore e flag) |
| 24 | STORE v | vedi §1.4; lascia il valore sulla pila (sp++) |
| 25 | EOS | inizio istruzione := pc; sp := bp |
| 28 | JMP rel | salto (non aggiorna l'inizio istruzione) |
| 29 | IFF rel | pop; se ≠0 riscrive `code[inizio istruzione]` in `JMP` al bersaglio falso e prosegue; altrimenti salta. Poi come JZ |
| 30 | CALL addr32 | `cella[bp].valore := pc di ritorno`; bp := bp+1; sp := bp; inizio istruzione := addr |
| 31 | RET | bp := bp−1; pc := `cella[bp].valore`; `cella[bp] := cella[sp−1]`; sp := bp+1 |
| 32–35 | POSTINC POSTDEC PREINC PREDEC v | ± solo se la cella ha flag int |
| 36 | INDEX | pop int come indice per l'istruzione **successiva** (PUSHV/STORE/±) |
| 37, 38 | SHR SHL | `a>>b` aritmetico, `a<<b` |
| 0, 26, 27, 39–255 | builtin | 0x412b30(op): 64..139 hanno un handler, gli altri danno "Unknown command" |

### 2.5 Celle e pila
Indice della cella `i` → indirizzo `LOGIC + 0x11c + 5*i` (flag) e `+0x11d` (valore). All'avvio di
ogni esecuzione `sp = bp = numero di globali`. La `CALL` iniziale salva l'indirizzo di ritorno 5
in `cella[ng]`, quindi `main` gira con `bp = ng+1`. La `RET` di `main` torna a 5, dove il `JMP
+30000` termina il frame.

### 2.6 Dichiarazioni (in 0x418140)
`int a, b=3, c[4]={1,2};` e `string s, t[3];`. Ogni elemento di array è una cella con lo stesso
nome (la ricerca trova la prima). Flag: int 2 (6 se array), stringa 1 (5 se array), `|8` se
inizializzata. Le stringhe ricevono un buffer da 128 byte vuoto. La lista di dichiarazioni
termina al primo carattere diverso da `,` (`;` nelle istruzioni, `;` o `)` nei parametri).

### 2.7 `extern`
`extern Nome(qualsiasi testo) ID;`: il testo tra parentesi è ignorato dal compilatore (serve solo
come documentazione), l'ID è letto con `%i` e troncato a un byte.

### 2.8 Altre stranezze del compilatore utili da sapere
- Il controllo "nome già usato" esiste solo tra variabili e tra funzioni; una variabile può avere
  il nome di un define (vince il define).
- In un argomento si può usare qualunque carattere come separatore al posto di `,`.
- `string s="x";` e `int x=COSTANTE;` non compilano.

### 2.9 Risultati del controllo statico su tutti gli script (`gscript.py check`)
Tutti i 39 script (`maps/*.ini`, `maps/levels/*.INI` con `18.ini`, `maps/net/*.INI`) compilano senza errori
(il controllo leggeva solo `*.INI` e saltava `levels/18.ini`: corretto con la issue #9).
Massimi: 20413 byte di codice (`levels/12.INI`), 243 celle (`net/05.INI`), 98 funzioni, 173 define.
- Nessuna chiamata a funzione utente dopo altri valori della stessa istruzione, nessun errore di
  pila (simulazione lineare della profondità), nessuna `iff` con chiamate utente nella condizione,
  nessun uso di precedenze che darebbero risultati diversi da C, nessuna lettura `s[i]` su stringhe,
  nessun letterale ottale, nessuna dichiarazione dentro funzione.
- 19 inizializzatori parziali di array (§1.3).
- 7 `iff` raggiungibili da un salto (§1.7): `HEADER.INI:751` (in `Tact`), `levels/01.INI:128`
  (dopo un `for` con `break`), `levels/12.INI:114`, `levels/15.INI:87`, `levels/18.ini:125`,
  `levels/19.INI:132`, `levels/27.INI:82` (dopo `if/else`).
- L'interprete C++ (`src/script`, issue #9) produce lo stesso bytecode e le stesse celle di
  `gscript.py` su tutti i 39 script (`tests/script_layout_compare.py`) e riproduce la patch delle
  `iff`: vedi `docs/script-language.md`.

---

## 3. Builtin (`extern` di `maps/HEADER.INI`)

Tutte nel dispatcher `0x412b30` (switch su `opcode−0x40`, tabella a `0x413f5c`); "handler" è
l'indirizzo del case. "Ritorno": **—** = non spinge nulla. Gli argomenti sono estratti dall'ultimo
al primo. "unit" = puntatore a unità (0 = nessuna). Le azioni `act_*` passano per il metodo
virtuale `unit->Action(act, var1, var2)` (vtable[0]) delle unità, documentato dalla issue #2.

| ID | Firma | Ritorno | Semantica | Handler | Conf. |
|---|---|---|---|---|---|
| 64 | `Load(string filename)` | — | Carica una mappa. Fuori dalla rete: `PostMessage(WM_COMMAND 40012, nome)`, cioè il caricamento avviene **dopo la fine del frame** (il resto di `main` continua). Il gestore copia il nome in 0x428068 e chiama 0x4149b0. Con `SetNet(≠0)` il nome è copiato in `obj+0x3446` e il caricamento è sincronizzato in rete. Un `.dem` avvia la riproduzione di una demo (§8). File inesistente = errore fatale | 0x412b5d | alta |
| 65 | `SizeTo(int unit,int x,int y)` | int | Distanza `max(abs(ux−x), abs(uy−y)*3>>1)` (0x40aff0); unit=0 → 60000 | 0x412bcc | alta |
| 66 | `Print(int i)` | — | Scrive l'argomento (come stringa) in `error.log` (debug) | 0x412c2b | alta |
| 67 | `Action(int unit,int act,int var1,int var2)` | int | unit=0 → 0. `act ≤ 15`: cambia azione/animazione (0x40b3e0(act,−1)) e restituisce 0. `act > 15`: restituisce `unit->Action(act,var1,var2)` | 0x412c51 | alta |
| 68 | `Create(int nvid,int x,int y,int z)` | int | `CreateUnit(nvid,x,y,z,dir=0,0,0)` (0x40e1b0): 0 se fuori mappa, se `MaxUnit` raggiunto o se il vid lo richiede sposta x,y di ±8 a caso (2 chiamate a `rand`). nvid fuori tabella = errore fatale | 0x412cc0 | alta |
| 69 | `GetTime()` | int | Orologio di gioco in ms (`0x42bf94`): parte dal `startTimer` della `.map`, avanza di `min(Δreale, 71)` per frame, fermo durante `Pause` | 0x412d0c | alta |
| 70 | `PlayAVI(string filename)` | — | `PostMessage(WM_COMMAND 40013)` → 0x401110: video MCI dopo il frame (§9) | 0x412d25 | alta |
| 71 | `LoadMenu(string filename)` | — | Aggiunge al menu corrente gli oggetti del `.men` (0x40c390, §5). Non svuota il menu. File assente: solo log | 0x412d37 | alta |
| 72 | `FreeMenu()` | — | Distrugge (`act_destroy`) tutte le unità della lista menu e la svuota (0x40c350) | 0x412d55 | alta |
| 73 | `ScrollLock(int flag)` | — | `obj+0x32d5 = flag`: ≠0 blocca lo scorrimento della visuale (bordi schermo), il vincolo attorno alla Kassandra e i tasti `+`/`-`. Azzerato a ogni caricamento mappa | 0x412d6b | alta |
| 74 | `MenuVid()` | int | nvid del pulsante cliccato nell'ultimo aggiornamento menu, 0 se nessun click | 0x412d83 | alta |
| 75 | `MenuDir()` | int | Indice di direzione del pulsante sotto il mouse/cliccato: `((dir_unità + vid[0x127]/2) & 0xff) * vid[0x45] >> 8`, dove `vid+0x45` è il numero di direzioni (es. 0..3 per `V_4BUTTON`) | 0x412daa | alta |
| 76 | `Exit()` | — | `PostMessage(WM_CLOSE)`: chiude il gioco | 0x412dc3 | alta |
| 77 | `PlayMidi(string filename)` | — | MCI `open %s type sequencer alias FWMUSIC` + play (non usata) | 0x412de2 | alta |
| 78 | `StopMusic()` | — | Ferma e chiude il CD (`stop/close FWMUSIC`, playlist annullata) e ferma un canale SFX (0x408d80 con il primo campo dell'oggetto audio) | 0x412e00 | alta / media (il canale) |
| 79 | `MaxUnit(int nvid,int maxunit)` | — | `vid[nvid]+0x128 = maxunit`: numero massimo di unità vive di quel vid (−1 = illimitato) | 0x412e27 | alta |
| 80 | `Effect(int effect)` | — | Effetto di schermo/palette `eff_*` (0x406e40); 255 = casuale 1..6; ignorato mentre è attivo `eff_openwindow` (6), che cattura la schermata attuale | 0x412e6e | media |
| 81 | `SetGraphDetail(int detail)` | — | Alto: abilita figli grafici extra (`VID_CHILD+14` = vid 199 per i vid 5, 63, 162, 193; `VID_CHILD+15` = vid 200 per il vid 74); basso: li azzera. In entrambi i casi `VID_CHILD+2`, `+4`, `+5` del vid 19 = vid 116 | 0x412e8c | alta (scritture) / media (significato) |
| 82 | `SetSound(int volume)` | — | Volume effetti 0–100 → `waveOutSetVolume(v*0xffff/100)` stereo; 0 = effetti disattivati | 0x412f18 | alta |
| 83 | `SetMusic(int volume)` | — | Volume CD 0–100 → `auxSetVolume`; 0 = musica disattivata; da 0 a >0 riavvia la playlist | 0x412f36 | alta |
| 84 | `SetCursor(int action)` | — | <0 nasconde il cursore; altrimenti lo mostra e imposta il tipo 0–15 (§6) | 0x412f54 | alta |
| 85 | `Flagman(int army)` | int | Flagman (unità del giocatore) dell'armata: `[0x439f9c + army*597]`, 0 se non c'è | 0x412f9b | alta |
| 86 | `GetObject(int nvid,int x,int y)` | int | Prima unità di quel nvid il cui rettangolo (centro ± semi-dimensioni del vid) contiene (x,y); 0 se nessuna (0x416840) | 0x412fca | alta |
| 87 | `AddCommand(int unit,int act,int var1,int var2)` | — | Accoda il comando `(act,var1,var2)` all'unità (0x40c7c0; ignorato se la coda è in `act_cyclecommand`) | 0x413008 | alta |
| 88 | `GetKey()` | int | Tasto del frame: codice `WM_CHAR` (13 Invio, 27 Esc, 9 Tab, ' ', '1'..'4', lettere) oppure 1 ←, 2 →, 3 ↑, 4 ↓, 5 Canc; 0 se nessuno. Vale per un solo frame | 0x41304e | alta |
| 89 | `Pause(int flag)` | — | ≠0: pausa (salva l'orologio e mette il cursore 0); 0: riprende, **riporta l'orologio al valore salvato** e sposta i timer delle unità (0x40ac70) | 0x41306a | alta |
| 90 | `Random(int max)` | int | `rand() % (max+1)` (LCG MSVC, 0..max) | 0x4130a4 | alta |
| 91 | `PlaySFX(int nsfx)` | — | Mette in coda l'effetto `nsfx` (1..numero di effetti in `fw.res`) a volume pieno, centrato (0x408dd0); richieste uguali nello stesso frame si fondono | 0x4130ca | alta |
| 92 | `SetGraphMode(int mode)` | — | 1/2/3 → `PostMessage(WM_COMMAND 40007/40010/40011)` → modalità 640×480 / 800×600 / 1024×768 dopo il frame | 0x4130ec | alta |
| 93 | `GetInputState()` | int | Bit `INPUT_*` del frame (§6) | 0x413ebb | alta |
| 94 | `GetUnitVid(int unit)` | int | nvid dell'unità (`unit+0x14`), 0 se unit=0 | 0x413162 | alta |
| 95 | `FirstUnit(int narmy,int typeunit)` | int | Inizia un'iterazione (stato globale) sulla lista delle unità **dall'ultima alla prima**, filtrando per armata **del vid** (`vid+0x38`) e `vid+0x22 & typeunit`; 0 se nessuna | 0x41318b | alta |
| 96 | `NextUnit()` | int | Unità successiva dell'iterazione, 0 alla fine | 0x413227 | alta |
| 97 | `PlayAudio(int t1,int t2,int t3,int t4)` | — | Playlist CD (§9) | 0x4132df | alta |
| 98 | `E_InsertTerrain(int x,int y,int nvid,int delflag)` | — | Inserisce/toglie terreno correggendo i bordi (funzione dell'editor, 0x4175f0) | 0x413319 | media |
| 99 | `FileCopy(string source,string dest)` | — | `CopyFileA(source, dest, FALSE)` | 0x41335b | alta |
| 100 | `Message(string text)` | — | Testo nella barra di stato, se caricata (act_settext sull'unità testo della barra) | 0x41337f | alta |
| 101 | `MenuAction(int vid,int dir,int action)` | — | Per ogni unità della lista menu con quel nvid e indice di direzione: `action<16` cambia azione, altrimenti `unit->Action(action,0,0)` (es. `act_destroy`) | 0x41339d | alta |
| 102 | `MenuCreate(int nvid,int dir,int x,int y)` | int | `CreateUnit(nvid,x,y,0,dir*256/ndir,0,0)`; se il vid è di classe 10 (pulsante) lo aggiunge alla lista menu (ordinata per priorità `vid+0x46`) | 0x41344c | alta |
| 103 | `strlen(string str)` | int | Lunghezza | 0x4134e5 | alta |
| 104 | `StateBar(int enable)` | — | Mostra/nasconde la barra di stato: carica `statebar.men` o `stateba8.men` (se lo schermo è largo 800) in una lista menu propria e crea un'unità testo (vid 127) per `Message` | 0x41350c | alta |
| 105 | `MenuSelectVid()` | int | nvid del pulsante sotto il mouse (anche senza click) | 0x41352a | alta |
| 106 | `SetGamma(int gammaindex)` | — | Imposta la palette/gamma di gioco (0x4074a0); ripristinata a 0x777 a ogni scaricamento mappa | 0x413543 | media |
| 107 | `SetWind(int wind,int direct)` | — | Vento: forza `0x439d54 = wind`, direzione `0x429e14 = direct`; default a ogni mappa 0 e 0x40 | 0x413561 | alta |
| 108 | `CreateLight(int x,int y,int z,int color)` | — | Sorgente di luce (renderer 0x407530) | 0x413586 | media |
| 109 | `GetX(int unit)` | int | `unit+0xc` (0 se unit=0) | 0x4135c8 | alta |
| 110 | `GetY(int unit)` | int | `unit+0xe` (0 se unit=0) | 0x4135ef | alta |
| 111 | `FWrite(int file,variable)` | — | Scrive il valore come testo + `"\n"` (file aperto in modo testo: CRLF) | 0x413616 | alta |
| 112 | `FRead(int file)` | str | Una riga (`fgets` 128) senza `\n`/`\r` finali. Se file=0 o fine file restituisce **la riga letta in precedenza** (buffer statico 0x43f8f0). In demo: letta dalla demo; in registrazione: salvata anche nella demo | 0x413651 | alta |
| 113 | `FOpen(string filename)` | int | `fopen(name,"r+t")`, 0 se manca (log). Sempre 0 durante una demo | 0x413770 | alta |
| 114 | `FClose(int file)` | — | `fclose` se ≠0 | 0x4137dd | alta |
| 115 | `FCreate(string filename)` | int | `fopen(name,"w+t")`. Sempre 0 durante una demo | 0x413801 | alta |
| 116 | `PrintText(int x,int y,string text,int font)` | int | Testo vuoto → 0. Altrimenti `CreateUnit(font,x,y,0)` + `Action(act_settext,text)`; restituisce l'unità testo | 0x413848 | alta |
| 117 | `PrintFile(int x,int y,string filename,int font)` | int | Come sopra con `act_setfile` (legge tutto il file) | 0x4138c5 | alta |
| 118 | `DeleteText(int text)` | — | `act_destroy` sull'unità testo (se ≠0) | 0x413937 | alta |
| 119 | `SetFlagman(int narmy,int unit)` | — | Imposta il flagman dell'armata | 0x41395e | alta |
| 120 | `Save(string filename)` | — | Salva la mappa corrente come `.map` (0x415620: sezioni 1, 2 con `act_save` per unità, 5, ...) | 0x4139a6 | media |
| 121 | `GetCommands(int unit)` | str | Coda comandi come testo: per comando `chr(act+60)` + `"var1,var2"` (buffer 0x43d2c8); unit=0 → buffer precedente | 0x4139c4 | alta |
| 122 | `SetCommands(int unit,string com)` | — | Imposta la coda dal testo (`%c%i,%i`) | 0x4139f8 | alta |
| 123 | `AskPlace(int vid,int x,int y,int z)` | int | Unità che impedisce di piazzare `vid` in (x,y,z), 0 se libero (0x40a2d0) | 0x413a24 | media |
| 124 | `GetVidName(int nvid)` | str | Nome del vid (inizio della struttura vid) | 0x413a6a | alta |
| 125 | `SaveDemo(string filename)` | — | Prepara la registrazione: la demo parte dal prossimo caricamento di mappa (§8) | 0x413a98 | alta |
| 126 | `SetPosition(int x,int y)` | — | Centra la visuale su (x,y): angolo = (x−larghezza/2, y−altezza/2) | 0x413ac3 | alta |
| 127 | `SetVid(int nvid,int type,int var)` | — | 1 `VID_MAXHP`: byte `vid+0x2d`; 2..17 `VID_CHILD+k`: word `vid+0xd7+2k`; 18..21 `VID_GAMMA+k`: palette alternativa k (0x406d60); 22 `VID_PROPERTY`: word `vid+0x24`; 23 `VID_WEAPONRANGE`: portata dell'arma n. `nvid` (`armi+9*n+4`); **24 `VID_NARMY` non è gestito** (nessun effetto) | 0x413afd | alta |
| 128 | `SetAutoScroll(int flag)` | — | `obj+0x32d1`: se ≠0 e il mouse non sta scorrendo dai bordi, la visuale insegue il flagman del giocatore locale (velocità = distanza dal centro, spostamento `v·dt/1024` per frame) | 0x413be4 | alta |
| 129 | `GetNetInfo(int type)` | int/str | 1 numero del giocatore locale; 2..5 nome del giocatore 0..3; 6 numero di giocatori; 9 azzera i contatori di uccisioni (**non spinge nulla**); 10..25 uccisioni `[(t−10)/4][(t−2)&3]`; altri tipi: nulla | 0x413bfc | alta |
| 130 | `Send(int data)` | — | Invia 4 byte a tutti (DirectPlay) | 0x413d2e | media |
| 131 | `Receive()` | int | 4 byte ricevuti, 0 se nulla | 0x413d58 | media |
| 132 | `SetNet(int flag)` | — | `obj+0x3557`: modalità rete per `Load` | 0x413d8f | alta |
| 133 | `GetNetKey(int nplayer)` | int | In rete: tasto del giocatore n; altrimenti `GetKey()` | 0x413da7 | alta |
| 134 | `StopSFX()` | — | Ferma tutti gli effetti (0x409550) | 0x413e01 | alta |
| 135 | `mciSendString(string command)` | — | Comando MCI grezzo | 0x413e17 | alta |
| 136 | `StopSFX(int nsfx)` | — | Ferma l'effetto nsfx (0x408d80). **Irraggiungibile** dagli script: il nome risolve sempre a 134 | 0x413e37 | alta |
| 137 | `PlayWAV(string file)` | int | Carica un WAV come effetto e restituisce il suo nsfx (0x408c50) | 0x413e55 | media |
| 138 | `GetNetInputState(int nplayer)` | int | In rete: bit di input del giocatore n; altrimenti `GetInputState()` | 0x413e7b | alta |
| 139 | `GetReg(string path,string name)` | str | `RegQueryValueEx(HKLM\path, name)` (128 byte), `""` se manca | 0x413ed7 | alta |

Funzioni dell'elenco della issue che **non sono builtin** ma funzioni script di `HEADER.INI`/
`music.ini`: `Tact`, `NextMap(string;int)` (avvia l'effetto e programma il cambio mappa dopo 700 ms
in `Tact`), `NextNMap(int)` (`maps\levels\NN.map`), `Win()`, `Lose()`, `SavePlayer()`,
`LoadPlayer()`, `GameMenuOn/Off()`, `MoveInMenu(int;int)`, `OptionsDraw/On/Tact()`,
`DesignMenuOn/Tact()`, `NetMenuOn/Off()`, `NetTact()`, `PlayMusic(int;int;int;int)` (chiama solo
`PlayAudio`).

---

## 4. Ciclo principale e tempi

- `WinMain` 0x401550: la riga di comando, se presente, sostituisce la mappa iniziale
  (`gromada.exe maps\levels\05.map` avvia direttamente quel livello); di default `maps\logo.map`
  (`maps\net.map` se lanciato da una lobby DirectPlay). Ciclo: finché non ci sono messaggi e la
  finestra è attiva e nessun video è in corso: 0x401f70 (orologio) → 0x4141f0 (tick) → 0x406ea0
  (disegno) → 0x402030 (presentazione) → azzera il tasto.
- Tick 0x4141f0: copia l'input in `obj+0x32dd` (12 byte), gestisce demo e rete, poi se
  `obj+0x32d9==0 && obj+0x991==0` (sempre vero nel gioco: sono campi dell'editor) esegue lo script,
  poi 0x40b880 (simulazione), 0x40ce50, menu 0x40c1d0, input giocatore 0x415e20, e alla fine
  azzera i bit di click (`&= ~3`) e il tasto.
- `Pause(1)` congela l'orologio; `Pause(0)` lo riporta al valore del momento della pausa (le demo
  mostrano un salto all'indietro di 10–20 s dopo il menu di inizio missione).
- `rand` (0x41c7b5) è l'LCG MSVC `x = x*214013 + 2531011; return (x>>16)&0x7fff`, inizializzato a
  ogni caricamento mappa con `timeGetTime()` (143 in rete, il seme registrato durante una demo).

## 5. Menu

- **Formato `.men`** (0x40c390, verificato su tutti i 14 file): 4 byte ignorati (0), poi 16 strati;
  ogni strato è una lista di oggetti `u16 nvid, i16 x, i16 y, i16 z, u8 direzione, u8 azione`
  terminata da `0xFFFF`. Dopo ogni oggetto il gioco chiama `Action(act_restore, FILE*, 0)`, che
  per le classi usate nei menu non legge nulla (in tutti i file gli oggetti sono di 10 byte esatti;
  `loadMenu()` in `map_loader.inl` è coerente). L'oggetto viene creato con `CreateUnit` e messo
  nella lista menu. Se la creazione fallisce si salta un byte.
- Le varianti `*8.men` (`player8`, `options8`, `design8`, `gamemen8`, `maps8`, `net8`,
  `stateba8`) sono per 800×600: centro in (400,300) invece di (320,240) e un oggetto di cornice in
  più (vid 83). Le sceglie lo script con `opt_resolution`; `StateBar` sceglie da solo in base alla
  larghezza dello schermo.
- Lista menu (`obj+0x970` = 0x43a6d8): `+0` numero, `+4` array, `+8` nvid selezionato, `+0xc`
  indice di direzione, `+0x10` click, `+0x18` unità selezionata. È ordinata per priorità del vid.
- Aggiornamento per frame 0x40c1d0, per ogni pulsante non disabilitato (azione 6
  `act_menudisable`, azione 15 o proprietà del vid `&0x20` esclusi): se il mouse (coordinate
  schermo) è sopra e il pulsante non è in `act_birth` (14): il primo pulsante sotto il mouse diventa
  il selezionato (`MenuSelectVid`, `MenuDir`); se c'è un click sinistro il click viene **consumato**
  (azzera i bit di input, quindi il gioco non lo vede), `MenuVid` lo riporterà al frame dopo e il
  pulsante passa in `act_menupressdown/up` (4/5). Altrimenti le azioni 0/1 (stand) diventano 2/3
  (select) sotto il mouse e tornano 0/1 fuori. Il bit basso dell'azione (down/up) è lo stato del
  pulsante a due posizioni.
- Gli script costruiscono i menu con `LoadMenu` + `MenuCreate(V_TEXTMENU, n, centr_x, centr_y)`
  (testo/cornice della voce n), navigano con `MoveInMenu` (↑/↓ via `GetKey()` 3/4) e
  `MenuSelectVid()`/`MenuDir()`, confermano con Invio, spazio o `MenuVid()`.

## 6. Input e cursori

- Il gestore della finestra 0x401840 mantiene uno stato a 12 byte (0x42bf88) copiato all'inizio di
  ogni tick in `obj+0x32dd`: `i16` mouse x,y nel mondo (schermo + visuale), `i16` mouse x,y sullo
  schermo (da `WM_NCHITTEST`), `u16` bit di input, `u8` tasto, `u8` non usato.
- Bit (`GetInputState`): `WM_LBUTTONDOWN` → `INPUT_LCLICK|INPUT_LDOWN` (0x11), `WM_LBUTTONUP` toglie
  `LDOWN`; destro uguale con 0x22/0x20; frecce ← → ↑ ↓ → 4/8/64/128 finché premute (`WM_KEYUP` le
  toglie); Shift → 0x100. I bit di click (1, 2) restano fino alla fine del tick che li vede.
- Tasto (`GetKey`): `WM_CHAR` (codice ASCII/ANSI del carattere), frecce → 1..4, Canc → 5.
  Azzerato alla fine di ogni tick. Un tasto o un click interrompe un video.
- Tasti gestiti dagli script (`Tact`): Esc (27) menu di gioco/uscita, Tab (9) barra di stato,
  '1'..'4' armi, Invio/spazio conferma, cheat `c h e a t` + lettera. Gestiti dal motore (0x415e20):
  `+`/`-` ingrandiscono/riducono la finestra di gioco (passi 27×20 px); mouse sui bordi (≤5 px dal
  bordo) scorre la visuale: la velocità (pixel ogni 1024 ms) cresce di 40 per frame fino a ±500 in x
  e di 30 fino a ±400 in y e si azzera appena il mouse lascia il bordo; spostamento per frame
  `v·dt/1024`; la visuale resta entro ±586×±440 dalla Kassandra. Con `ScrollLock(1)` niente di
  tutto questo (0x415e20, alta).
- **Guida della Kassandra**: la classe del flagman (funzione 0x411a2c, chiamata dalla simulazione)
  legge direttamente l'intero record di input del suo giocatore (`obj+0x32dd` in locale,
  `0x439fa8 + 597*n` in rete): posizione del mouse nel mondo e bit dei pulsanti/frecce. Il movimento
  col sinistro e il fuoco col destro descritti nella issue stanno lì (dominio della issue #2; le demo
  mostrano il tasto destro tenuto per lunghi tratti).
- Comandi "da strategico" (0x415e20, dopo l'aggiornamento menu; confidenza **media**: flusso letto
  nel disassemblato, semantica delle funzioni chiamate non verificata). C'è una lista di unità
  selezionate (`obj+0x964`, di norma la sola Kassandra) e un'unità sotto il cursore (0x416900):
  - click sinistro con selezione: per ogni unità selezionata, se sotto il cursore c'è un'altra
    unità → `AddCommand(act_attack, bersaglio)`; altrimenti `act_move` verso il punto (0x40c690:
    se il punto è entro 24×16 px dalla posizione dell'unità o da un `act_move` già in coda, il
    comando viene accodato insieme ad `act_cyclecommand`, cioè un pattugliamento ciclico);
  - click sinistro senza selezione: seleziona l'unità sotto il cursore (`act_select`) e inizia un
    rettangolo di selezione trascinato col tasto tenuto;
  - tasto destro: deseleziona tutto e traccia col tasto tenuto un poligono (0x40cc00); al rilascio
    seleziona le unità al suo interno (selezione "a lazo").
- Cursori: tabella di 16 a 0x440948 caricata da 0x4179d0: **0 `ARROW.CUR`, 2 `MOVE.CUR`, 5
  `FIGHT.CUR`, 8 `NOMOVE.CUR`, 9 `CYCLE.CUR`**, gli altri la freccia di sistema (alta). Scelta per
  frame (media): nessuna selezione → 0; unità sotto il cursore di un'armata (del vid) diversa da
  quella della prima selezionata → 5; altrimenti 8 se il punto non è libero (0x40a430), 2 se lo è;
  2 o 5 diventano 9 se un `act_move` lì creerebbe un pattugliamento ciclico (0x40c4b0).
  Con l'opzione "cursori di Windows" spenta il cursore è uno sprite e il tipo è la sua azione.
  `SetCursor(-1)` lo nasconde.

## 7. Salvataggi

- **`save1`..`save5`** (profilo giocatore, testo CRLF, una riga per valore) sono scritti dalla
  funzione script `SavePlayer()` e letti da `LoadPlayer()`: `player_name`, `map` (bitmask delle
  missioni vinte), `nweapon[0]`, `hp`, `noammo[0..7]`, `weapon[0..3]`, `opt_player` (bit 1 barra di
  stato, 4 principiante), `kassandra` (vid della trasmissione), `upgrade` (bitmask). 19 righe.
  `PLAYER.INI` crea i profili nuovi (nome `STR_NONAME` = "SENZANOME" nell'edizione italiana).
- **`fw.ini`** (opzioni, scritto da `OptionsTact` in `HEADER.INI`, letto da `mainmenu.ini`):
  `opt_resolution` (0 = 640×480, 1 = 800×600), `opt_detail`, `opt_sound`, `opt_music`,
  `opt_scrolling`.
- Non esistono salvataggi di partita a metà missione: `Save(file)` (0x415620) scrive una mappa
  intera in formato `.map` (sezioni MapInfo con l'orologio corrente, oggetti con `act_save` per
  unità, ID, comandi, armate), è la funzione dell'editor; nessuno script la chiama.
  `act_save`/`act_restore`/`act_fullsave`/`act_saveframe` sono azioni delle unità che scrivono e
  leggono il proprio stato in quel formato (dettagli per classe nella issue #2 e in
  `map_loader.inl`/`map_saver.inl`).

## 8. Demo (`maps/demo*.dem`) — formato decodificato

Le demo sono **registrazioni dell'input**, riprodotte rieseguendo il gioco in modo deterministico.
Formato (0x4149b0, 0x4141f0, 0x413651, 0x415c00), confidenza **alta**: il parser
(`src/demo/demo_file.hpp`, `tools/re/runtime/gdemo.py`) legge le tre demo arrivando esattamente
al marcatore di fine, con il numero di globali calcolato dal compilatore replicato.

- Contenitore `.res` con una sola sezione di tipo 7. Dati: `u32 lunghezza, nome mappa`, `u32 seme`
  di `srand`, `u32 orologio` iniziale, `u32` valori di **tutte le celle int dello script** subito
  dopo la compilazione (157 per il livello 10, 146 per 06 e 14), poi per ogni tick
  `u32 orologio + 12 byte di input` (§6), con in mezzo i record `u32 lunghezza + testo` di ogni
  `FRead` eseguita durante quel tick (19 righe di `LoadPlayer()` al primo tick), e infine
  `u32 0xFFFFFFFF`. Dopo la sezione c'è il nome della mappa da caricare alla fine
  (`maps\mainmenu.map`): non è scritto dall'eseguibile (probabilmente aggiunto a mano).
- Registrazione: `SaveDemo(file)` apre il file; al caricamento della mappa successiva viene scritta
  l'intestazione, a ogni tick l'input, e allo scaricamento della mappa il marcatore.
- Riproduzione: `Load("maps\demoN.dem")` (in `mainmenu.ini` dopo 30 s di inattività, N = 1..3 a
  caso) riconosce la sezione 7, carica la mappa indicata con il seme, l'orologio e le globali
  registrati, e a ogni tick sostituisce l'input. Durante la demo `FOpen`/`FCreate` restituiscono 0
  e `FRead` legge dalla demo. **Un tasto o un pulsante premuti davvero** interrompono la demo
  ("exit"), come la fine dei dati. Quando lo script chiede un altro `Load`, il gioco cerca il
  marcatore e carica la mappa del trailer.
- Bug dell'originale (alta): la ricerca del marcatore chiama `fseek(f, 2, k)` (origine non valida)
  e quindi legge in avanti dalla posizione corrente a passi di 4 byte; funziona perché i record di
  tick sono allineati a 4 byte rispetto alla posizione da cui parte la ricerca.
- Uso come test di regressione: richiede una simulazione deterministica identica (stesso `rand` e
  stesse chiamate, stesso orologio). I 12 byte di input per tick e i 19 `FRead` bastano a
  pilotare l'interprete e il motore; il parser fornisce tutto.

## 9. Audio e video

- `PlayMusic(m1,m2,m3,m4)` (script) → `PlayAudio` → 0x409230. Le tracce sono **numeri di traccia
  del CD audio** (formato MCI `tmsf`, la traccia 1 è quella dati; gli script usano 5, 8, 10, ...).
  Se è già in corso la stessa quaterna non fa nulla (non riavvia); valori negativi in t2..t4 sono
  sostituiti da t1; t1 < 0 non fa nulla. Suona `from t to t+1` (una traccia) e alla notifica di
  fine traccia (`MM_MCINOTIFY`) passa alla successiva con la regola
  `next = cur==t1 ? t2 : cur==t2 ? t3 : cur==t3 ? t4 : t1` (prima corrispondenza: `8,9,8,9`
  alterna 8 e 9, `5,5,5,5` ripete la 5). Quando la finestra perde il fuoco il CD viene fermato e
  poi ripreso sulla traccia corrente (0x4095c0/0x4095e0, confidenza media). Menu: `PlayMusic(10,10,10,10)`.
- `StopMusic()` ferma il CD. Volumi con `SetSound`/`SetMusic` 0–100 lineari (0 disattiva).
- Effetti: `PlaySFX(n)` con n indice degli effetti di `fw.res` (1-based).
- Video (0x401110): `PlayAVI` mette in pausa il CD, pulisce lo schermo, apre il file con MCI in una
  finestra figlia centrata alla dimensione nativa, nasconde il cursore e blocca il ciclo di gioco
  finché il video finisce o si preme un tasto/pulsante; poi riprende la musica. Se il file non si
  apre si continua senza video.
  - `avi/0.avi` (640×480, 15 fps, MS Video 1): logo, primo frame di `logo.ini`.
  - `avi/1.avi` (Indeo 3): intro, al primo ingresso nel menu principale (non dopo la missione 27).
  - `avi/2.avi` (Indeo 3, 2795 frame): uscendo dal menu principale verso partita o crediti
    (non per le demo, non dopo la missione 27).
  - `avi/3.avi` (Indeo 3): finale, in `win.ini` dopo la missione 27 (forza 640×480).

## 10. Testi e codifiche

- Le unità testo (classe font, vid `V_FONT` 127; 0x40dea0) tengono una stringa e un numero di
  caratteri visibili. `PrintText` (`act_settext` 71) mostra subito tutto; `PrintFile`
  (`act_setfile` 73) legge l'intero file; `Action(t, act_settextcount, n, 0)` (32) fa ripartire
  l'effetto macchina da scrivere da n caratteri: a ogni passo (azione 45) un carattere in più, gli
  spazi vengono saltati in blocco e dopo un a capo c'è una pausa di 150.
- Disegno (0x40b620): font a larghezza fissa (`vid+0x27` avanzamento, `vid+0x29` altezza riga);
  ogni byte ≥ 0x20 è l'indice del fotogramma del font; `\n` va a capo, `\r` torna a inizio riga;
  **ESC (0x1b) seguito da un byte cambia font con quel nvid** (un solo byte, non due cifre come dice
  il commento dell'header). Nessun file dell'edizione italiana usa ESC.
- Codifica: i byte sono indici di glifo, quindi la codifica è quella del font, che segue
  **CP866** (l'unica stringa non ASCII dei testi italiani, in `levels/09S.TXT`, è "АРЕНА" in CP866,
  rimasta dalla versione russa). I testi italiani sono ASCII maiuscolo, con gli accenti scritti come
  apostrofo ("UNITA'"). I commenti degli script sono in CP866.
- File: `maps/LOCALE.INI` = `#define` delle stringhe dell'interfaccia (`STR_NONAME`, `STR_NONE`,
  `STR_ON`, `STR_OFF`, `STR_HIGH`, `STR_LOW`) inclusi da `HEADER.INI`; `maps/levels/NN.TXT` =
  briefing mostrato da `GameMenuOn` (`PrintFile`); `maps/levels/NNS.TXT` = descrizione breve nella
  scelta mappa (`MAPS.INI`); `maps/W01..W15.TXT` = schede delle armi nel centro servizi
  (`DesignMenuTact`); `credits.ini` = script della mappa dei crediti.

## 11. Per la issue #2

- Ordine del frame e orologio: §4 (clamp 71 ms, `Pause` riporta indietro l'orologio e sposta i
  timer delle unità a `+0x1e`).
- `rand`/`srand` e seme per mappa: §4. `CreateUnit` (0x40e1b0) chiama `rand` due volte per i vid con
  `vid+0x25 & 1` (spostamento ±8 in x e y) prima di tutto il resto; restituisce 0 per nvid con
  `MaxUnit` raggiunto (`vid+0x128` massimo, `vid+0x12c` contatore) o fuori dalla mappa; il
  costruttore dipende da `vid+0x23` (classe, ≤ 20).
- Strutture (offset confermati dagli handler): unità `+0xc` x, `+0xe` y (i16), `+0x12` direzione,
  `+0x13` azione, `+0x14` nvid, `+0x1e` timer; vid a `[0x42df57] + 310*nvid` con `+0x22` maschera
  tipo (`u_*`), `+0x23` classe, `+0x24` proprietà, `+0x25` flag, `+0x2d` hp massimi, `+0x38`
  armata, `+0x45` numero di direzioni, `+0x46` priorità menu, `+0xd7+2k` figli, `+0x128` max unità;
  armi a `[0x439d3c] + 9*n` con `+4` portata; armate a `0x439d68+...` con passo 597 (flagman a
  `0x439f9c`).
- `Action(unit, act<=15, ...)` = cambio azione con direzione −1 (0x40b3e0) senza chiamare il
  metodo virtuale; `act>15` = `vtable[0](act, var1, var2)`.
- Coda comandi (0x40c7c0): `u16 n, u16 capacità`, record da 9 byte `{u8 act, u32 var1, u32 var2}`;
  i nuovi comandi entrano in testa; se il comando in testa è `act_cyclecommand` (61) i nuovi
  vengono ignorati.
- `SizeTo`/`GetObject`/`FirstUnit`: §3. `SetWind`: forza `0x439d54`, direzione `0x429e14`.
- Il controllo della Kassandra è in 0x411a2c, che legge il record di input a 12 byte del giocatore
  (§6): mouse nel mondo `+0`, bit `+8`.
- Le demo (§8) sono il test di regressione naturale per la simulazione.

## 12. Domande aperte

- Comandi del giocatore col mouse (0x415e20) e scelta del cursore 9: descritti a confidenza media;
  da chiudere con la issue dell'input.
- Effetti `eff_*` (0x406e40, 0x405700), `SetGamma`, `CreateLight`: comportamento del renderer non
  analizzato in dettaglio.
- Significato di `obj+0x32d9`/`obj+0x991`/`obj+0x32eb` (solo azzerati e letti: probabilmente
  modalità dell'editor) e del campo `+8 == 100` degli effetti sonori in 0x408dd0.
- `StopMusic` ferma anche "un" canale SFX: quale sia (forse l'ultimo `PlayWAV`) non è verificato.
- Il trailer delle demo non è scritto dall'exe: forse aggiunto da uno strumento esterno.

## 13. Strumenti (`tools/re/runtime/`)

Richiedono la cartella del gioco in `GROMADA_DATA` (default `/root/gromada-data/game`) e non
salvano nulla nel repo. Gli script angr usano il venv `/root/tools/angr-venv`
(`python3 -m venv ... && pip install angr capstone pefile`).

- `grre.py`: carica l'exe in angr e costruisce il CFG; `dec.py ADDR...` decompila in
  `$GRRE_CACHE/dec/`; `funcs.py START END` elenca funzioni con chiamanti e stringhe;
  `callers.py ADDR...` chiamate a una funzione; `fulldis.py` disassembla tutti i blocchi noti
  (per cercare offset di campi).
- `gdis.py START END|+LEN`: disassemblatore lineare (solo capstone + pefile) con stringhe annotate.
- `gbuiltins.py [ID...]`: disassembla gli handler delle builtin 64..139 con i nomi di `HEADER.INI`.
- `gscript.py compile FILE [--dis]` / `gscript.py check`: replica del compilatore, disassemblatore
  del bytecode e controllo statico di tutti gli script (§2.9).
- `gdemo.py maps/demoN.dem [--ticks]`: parser delle demo (usa `gscript.py` per contare le globali).
- `src/demo/demo_file.hpp` + `tests/demo_file_test.cpp`: parser C++ header-only delle demo (trova da
  solo il numero di globali) con test sintetico e test sui dati originali.
