# Blood Strike (PC / Steam) — triage do alvo

---

## 0.6 O SWEEP DE FUNDO E O POOL QUE NUNCA CRESCIA (2026-10-17, última sessão)

Esta secção é a **última** e substitui a 0.5 onde houver conflito. A 0.5 continua
correcta no frontend, no aim e no perf; o que muda aqui é a descoberta de
candidatos.

### O sintoma, dito com os números do log

Duas corridas seguidas em partida real produziram isto:

```
[pool] 117615 pass the +0x3E0 signature, 1414 also have a world position
[pool] skeleton filter: 462 rejected with no bone pair, 942 with a non-human
       bone shape, 9 with the root away from the feet
[pool] 1 candidates kept
[live] tracked 1  within 200m 0  on-screen 0  nearest -1.0m
```

`1 candidates kept` e `tracked 1`. E na corrida anterior, idêntico, com `2`.

### A causa — e não era o filtro nem os ossos

Os ossos estão certos e estão provados: a linha `[body]` imprime

```
[body] nearest 37.0m  pos.y 0.266  root.y 0.150  head.y 2.424
       pos-root 0.116  head-pos 2.158
```

`pos-root 0.116` (pés → raiz do esqueleto = 11,6 cm) e `head-pos 2.158` (cabeça a
2,16 m dos pés) são exactamente os valores que a 0.3 fixou como prova. O filtro
também está correcto: deixa passar o actor real.

O problema é **quando** a varredura corre. Do log, na ordem em que foi escrito:

```
[pool] ... 1 candidates kept
[match] entered a match at 11.60 0.27 -35.50
```

A varredura corre **no instante da entrada na partida**, quando o mapa está
praticamente vazio. Nesse instante existe um jogador real — o operador — e é esse
o único candidato. Depois a partida enche-se, mas `pool_refresh_ms` valia `0`,
portanto **o pool nunca era reconstruído** e o conjunto de rastreio não conseguia
crescer para lá daqueles 1–2 endereços. Todo o resto da partida era invisível.

### Porque é que a correcção óbvia era a errada

A correcção óbvia — pôr `pool_refresh_ms` a 30000 — já tinha sido tentada e
revertida, e a razão está documentada no próprio código: uma varredura lê ~2 GB e
bloqueia **40–60 s** na thread de desenho. Seis segundos de freeze a cada trinta
não é lag, é um stall. Foi reportado como "as caixas param e depois voltam".

As duas opções — varrer sempre, ou nunca varrer — eram ambas erradas porque a
premissa era errada. **Uma varredura de 2 GB não pertence à thread que tem de
apresentar um frame a cada 16 ms, com nenhum intervalo.**

### A correcção: o sweep mudou de thread

`bs_esp.cpp` ganhou uma thread de sweep (`sweep_thread_main`), criada em
`start()` e terminada em `stop()` **antes** de `g_h` ser fechado. A propriedade
que a torna segura é uma inversão:

- a thread de fundo **não escreve** `pool_` nem `last_pos_`; lê o jogo e publica
  uma lista de endereços num inbox (`sweep_inbox_`) protegido por um mutex que
  só guarda o inbox;
- a thread de desenho, no topo do `poll_pool`, **consome** esse inbox e faz o
  merge ela mesma (`merge_sweep_inbox`).

Logo `pool_` e `last_pos_` têm **exactamente um escritor**. Não há invariante
para partir, e não há lock a segurar durante um scan.

Sincronização: dois eventos manuais-reset. `sweep_event_` pede uma varredura;
`sweep_stop_` termina a thread. O worker espera nos dois com
`WaitForMultipleObjects`, para que um stop seja honrado de imediato mesmo sem
varredura pedida — é isso que torna o join determinístico. Um pedido que chegue a
meio de um scan fica pendente e é honrado no fim, nunca em paralelo.

O merge mantém a semântica que a 0.5 estabeleceu: um endereço já vigiado
sobrevive **com o seu histórico de movimento** (sobrescrever a baseline apagaria
a evidência que a promoção espera), só os novos são acrescentados, e o cursor é
rearmado porque um índice numa lista re-ordenada não significa nada.

O único lock é um `push_back` de um vector; a secção crítica é de microssegundos.

### O portão: não varrer no lobby

O primeiro sweep de fundo correu 20 s após a entrada, altura em que o operador já
tinha morrido e voltado ao menu. O resultado foi

```
[pool] background sweep: 104958 pass the signature, 1023 have a position,
       0 survive the skeleton test
```

`0` é a resposta **correcta** à pergunta errada: num menu não há jogadores. Duas
gigabytes de leituras e cem mil testes de assinatura para produzir nada. O
`rescan_if_due` passou a sair cedo com `waiting_for_match_`, que é exactamente o
sinal de "menu, morto, espectador, ou antes da partida".

### Provado no log, nesta sessão

```
[pool] scheduling a merge sweep (drawing continues)
[pool] background sweep started
[pool] background sweep 9336325 raw candidates from 2227 MB scanned
...
[live] fps 73  ...   (durante o scan)
```

A thread de fundo varre 2227 MB **e o `[live]` mantém fps 73**. Era isto que
faltava. O `[start] background sweep thread up` confirma o arranque, e o
`[DEATH/SPECTATE -- blanked]` continua a funcionar (fov 54.6 → 40.0).

### O que continua por resolver

`tracked 1  within 200m 0  nearest -1.0m` — o único actor tem distância
inválida e o `[body]` desta build **nunca chegou a imprimir**. Três candidatos e
nenhum com posição utilizável. Duas hipóteses, distinguíveis:

1. o `build_pool` inicial corre cedo demais, no instante em que a cadeia do
   `local_` ainda não estabilizou (noutras corridas leu `(0,0,0)` no arranque);
2. a assinatura `+0x3E0 == 2` já não é a certa — `104958` de 9,3 M é 1,1%, e
   desses só 1023 têm posição, 0,0097%. Isso é ruído, não uma assinatura.

Se o `[body]` aparecer com `pos-root 0.116` e `head-pos 2.158` quando há mesmo
alguém vivo, a hipótese 1 é a certa e é uma questão de temporização. Se não
aparecer, é a 2.

### Ficheiros tocados nesta sessão

| Ficheiro | O que mudou |
|---|---|
| `bs_esp.hpp` | `#include <mutex>`; declarações `sweep_thread_main`, `sweep_thread_entry`, `request_sweep`, `sweep_running`, `merge_sweep_inbox`; membros `sweep_thread_/event_/stop_`, `pool_mtx_`, `sweep_inbox_`, `sweep_inbox_ready_`, `sweep_active_`, `sweep_pending_`; `pool_refresh_ms` 0 → 20'000 |
| `bs_esp.cpp` | `CH` movido para o anónimo; `build_pool(merge)`; `sweep_thread_main` e os seus helpers; `merge_sweep_inbox`; `rescan_if_due` a pedir em vez de chamar, com o portão `waiting_for_match_`; `merge_sweep_inbox()` no topo do `poll_pool`; thread criada em `start()`, terminada em `stop()` antes de `g_h` |

---

## 0.5 FRONTEND, AIM ASSIST E PERFORMANCE (2026-10-17, sessão anterior)

Esta secção é a mais recente e descreve o que está no binário agora. As secções
0.4 e anteriores descrevem o caminho que levou até aqui e continuam válidas como
registo, mas onde houver conflito, **esta manda**.

### Menu e estado visual — `menu.hpp` / `menu.cpp`

O menu é um módulo próprio, não um punhado de widgets ImGui. A razão é o
estado: existe **uma só** configuração (`UiState`, em `menu.hpp`), escrita pelo
menu e lida pelo render do ESP. Duas cópias do mesmo campo é a forma clássica de
um switch não fazer nada — o menu edita uma, o render lê a outra — e o sintoma
("o botão não funciona") não aponta para a causa.

`UiState` divide-se em três, e a divisão é deliberada:

| Campo | Quem escreve | Quem lê |
|---|---|---|
| `EspVisual` | menu | render do ESP |
| `AimAssist` | menu | simulação; **ninguém** o liga a input real ainda |
| `MenuState` | input | o próprio menu |

O tema está em `namespace theme`, com os valores hex do documento lado a lado
com o código, para poder ser comparado linha a linha com a especificação.

O ImGui é usado como biblioteca de desenho, não como framework de widgets: a
moldura, a barra lateral, os cartões e os switches são desenhados com listas de
desenho, porque re-estilizar cada widget do ImGui para uma paleta de 34 cinzas
dá mais código do que desenhá-los e luta com o framework em cada upgrade. O que
vem do ImGui é o **encaminhamento de input** (`InvisibleButton` + `IsItemHovered`
/ `IsItemActive`), porque hit-test, sobreposição e arrastar de slider são
exactamente as partes que não vale a pena reimplementar.

**Bug de layout corrigido antes de sair**: as alturas dos cartões estavam escritas
como `16 + 18 + kRowH * 5 * scale + 16` — mistura de pixels de dispositivo
(não escalados) com linhas escaladas por DPI. A 150%, que é o padrão do Windows
em portátil, cinco linhas precisam de 292 px e a constante reservava 211. Agora
cada altura é **derivada das suas próprias linhas**, e adicionar uma linha faz o
cartão crescer.

### Input do menu com o rato — `feed_mouse`

O overlay é `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` e tem de
continuar assim: é a razão pela qual o input do jogo funciona. Não recebe
`WM_MOUSEMOVE` útil e **nunca** pode chamar `SetCapture`.

O menu precisa de posição e botão, e ambos existem process-wide:
`GetCursorPos` e `GetAsyncKeyState`, entregues ao ImGui como eventos de input
brutos (`AddMousePosEvent` / `AddMouseButtonEvent`). `ImGuiConfigFlags_NoMouse`
**continua ligado**, portanto o backend nunca toca no cursor do sistema.

Três detalhes que são fáceis de errar e estão resolvidos no código:

1. **A ordem.** O `feed_mouse` corre **antes** de `begin_frame()`. O
   `NewFrame()` tranca a fila de input do frame que vai ser construído; eventos
   adicionados depois ficam para o frame seguinte, o que num arrastar se lê como
   o slider atrasado e num clique como o primeiro clique engolido.
2. **As coordenadas.** ImGui usa o **cliente** do overlay (o backend faz
   `GetClientRect` todos os frames), portanto a conversão é `ClientToScreen` e
   não `GetWindowRect` — que fica uma borda errado no momento em que algo dê
   moldura à janela.
3. **O `ClipCursor`.** O jogo prende o rato com `ClipCursor`, e um rato preso
   nunca alcança o menu. `ClipCursor(NULL)` é libertado **na transição** de
   abrir, não a cada frame — um `ClipCursor(NULL)` por frame é uma perturbação
   por frame exactamente do estado que se provou quebrar o input. Ao fechar não
   é restaurado: restaurar exige o rect do jogo, que não é legível, e um rect
   adivinhado prende o rato na região errada. O jogo re-afirma o clip sozinho
   no próximo input que processa.

### O menu funciona no lobby — e isso era um bug

O `draw_frame` tinha um `return` antecipado em `waiting_for_match_`, o que
tornava o menu **inalcançável** exactamente onde mais é querido: no lobby, antes
da partida. Pressionar a tecla do menu não fazia nada, e não fazia nada em
silêncio. Agora o frame é desenhado na mesma, com o ESP suprimido (não há nada
válido para projectar) e o **menu desenhado por cima**, porque é a única coisa
que não precisa de estado de jogo nenhum.

### Aim assist — `aim.hpp` / `aim.cpp`

Assistência **proporcional**, não lock. Só injecta enquanto a tecla de activação
está premida **e** o utilizador está fisicamente a mover o rato. Largue a tecla,
pare de mexer, ou perca o alvo, e não faz rigorosamente nada.

Três pontos de integração existiam e dois foram rejeitados, pelos mesmos
motivos — são absolutos, e absoluto é um lock:

- **Escrever o ângulo na memória do jogo** — rejeitado. Um lock duro, visível
  como descontinuidade, e tira o operador do ciclo.
- **Injectar posição absoluta do rato** — mesma objecção: um evento que teleporta
  a vista lê-se como o que é.
- **Injectar movimento RELATIVO proporcional** — o que está feito. É o mesmo
  mecanismo que um rato físico produz (`SendInput` + `MOUSEEVENTF_MOVE`), e como
  a magnitude é uma fracção do erro restante e está condicionada ao utilizador já
  estar a mexer, lê-se como um rato mais fácil de controlar.

**Não depende de offsets novos.** A base da câmara em `+0x41C` já é lida todos
os frames para o ESP, e o chamador projecta cada candidato com o mesmo
`world_to_screen` das caixas. O módulo recebe **pontos em coordenadas de ecrã** e
não sabe o que é um `Actor` — consequência deliberada: a assistência nunca pode
ser mais precisa do que a projecção, e o módulo testa-se contra pontos sintéticos
sem jogo nenhum a correr.

Portões, e a lista tem de ser exaustiva porque é aí que está a segurança: ESP
ligado, projecção válida, `!blank_only` (jogo em primeiro plano **e** câmara de
mira do jogador, não killcam), menu fechado, tecla premida, rato a mexer.

**Validado por dados simulados antes de tocar em nada real** — `src/aim_test.cpp`,
selecção (mais próximo dentro do raio, fora de ecrã excluído, fronteira do raio) e
suavização (convergência monótona, sem overshoot, zona morta, clamp por frame).
O teste apanhou um erro no **próprio teste** (um alvo "fora de raio" que estava a
40 px do centro), o que é precisamente a distinção que o ficheiro existe para
fazer.

### Performance — o que mudou e porque

**1. A superfície GDI do `present_cpu` é criada uma vez, não por frame.**

O código anterior chamava, dentro do `present_cpu`, 60 vezes por segundo:

```
GetDC(nullptr) · CreateCompatibleDC · CreateDIBSection (~8 MB a 1080p)
DeleteObject · DeleteDC · ReleaseDC
```

Uma alocação de 8 MB e a libertação correspondente, por frame, mais quatro
operações de handle GDI para o kernel. Nada disto precisa de acontecer mais de
uma vez: a superfície tem o mesmo tamanho até a janela redimensionar, e é
totalmente reescrita em cada frame. Criada uma vez, guardada, libertada só em
`destroy()` ou quando o tamanho do cliente muda.

**2. O premultiply é vectorizado, e testado exaustivamente.**

`present_cpu` fazia um ciclo escalar sobre ~2 M píxeis. Agora usa SSE2 (linha de
base em x64, sem verificação em runtime), fusa com o empacotamento porque o
`RowPitch` do staging é dependente do driver e o DIB é compacto. O caminho rápido
para blocos totalmente opacos é uma cópia, e blocos totalmente transparentes
colapsam para zero — e o overlay é maioritariamente transparente.

**`src/premul_test.cpp` encontrou um bug real nesta mudança**, na primeira
execução: a difusão do alfa usava `_mm_shuffle_epi32`, que replica a metade alta
de 64 bits sobre a metade baixa e sobrepõe o alfa correcto do pixel 0 com o do
pixel 1. Resultado: os dois primeiros píxeis de cada bloco saíam errados. Nenhum
screenshot o mostraria — é um erro subtil na borda de cada píxel semi-transparente.
O teste compara **18 432 casos** contra a referência escalar; 0 falhas depois da
correcção.

**3. `EnumWindows` fora do ciclo de render.**

`game_window()` anda por **todas** as janelas top-level do sistema, e era chamada
**todos os frames** só para ler o tamanho do cliente — sessenta varreduras do
desktop por segundo, para um valor que só muda quando o jogador muda de resolução
ou alterna fullscreen. O handle passa a ser encontrado uma vez e guardado, com
refresh só quando deixa de ser uma janela; o `GetClientRect` continua por frame,
que é barato.

### Testes

Dois alvos de consola, sem dependência do jogo nem de D3D:

| Alvo | O que verifica | Casos |
|---|---|---|
| `premul_test` | premultiply SIMD vs referência escalar | 18 432, 0 falhas |
| `aim_test` | selecção e suavização do aim assist | 0 falhas |

Correm com `cmake --build build --config Release --target premul_test aim_test`
e depois `out\premul_test.exe` / `out\aim_test.exe`.

### O que fica pendente, e dito como pendente

- **Nomes e barra de vida no ESP.** Não há offset medido para nenhum dos dois. Os
  interruptores existem no menu, **desactivados e rotulados** ("Nomes e vida: em
  desenvolvimento"), para o buraco ser visível em vez de silenciosamente ausente
  — e para nunca se inventar um nome ou 100% de vida.
- **Skeleton.** Continua suportado e desligado por omissão, como a especificação
  pede para esta etapa. Os ossos ainda não estão medidos (`+0x200` vs
  `+0x208`/`+0x214`, stride 12), portanto continua a desenhar a espinha a partir
  do primeiro troço do array.
- **Movimento real do aim assist.** A estrutura, os portões e a matemática estão
  prontos e testados; o passo que injecta está implementado. **Não foi testado
  com o jogo a correr** — não houve UAC nesta sessão. É o primeiro item a validar
  quando o jogo abrir: selecção, suavização e ausência de movimento com o
  recurso desligado.
- **Persistência de settings.** Não há: restaurar padrão é em memória. Um
  ficheiro de configuração é uma decisão à parte e não foi tomada.

---

## 0.4 QUATRO BUGS DO ESP (2026-10-17) — projecção, poll, promoção, janela

Todos encontrados com o ESP em execução e o jogador vivo. Todos têm prova nos
números, não em raciocínio. Ficam aqui porque cada um custou corridas.

### Bug 1 — a matriz da câmara era lida com o espaçamento errado

O 3x3 em `+0x41C` é **denso**: as linhas estão a **3 floats** de distância
(`+0`, `+3`, `+6`). O `world_to_screen` lia a **4 floats** (`+0`, `+4`, `+8`),
como se fosse um 4x4 com padding. Cada linha era lida deslocada um elemento —
a "linha 1" era o fim da linha 0 mais o início da linha 1.

A prova está no eixo vertical. Lido com espaçamento 3:

```
row1 = (0.00000, 0.99999, 0.00529)     <- (0,1,0). É o eixo vertical.
```

Lido com espaçamento 4 seria `(0.99999, 0.00529, ...)`, que não é vertical. O
eixo "para cima" de uma câmara **tem** de ser vertical neste mundo (y-up). Não
é uma questão de gosto — é a única leitura que produz uma base de câmara.

O dump dos 12 floats crus passou a ser impresso para isto nunca mais ser uma
suposição. Os três últimos floats `(+32.8, -1.5, -1.9)` são a translação.

### Bug 2 — `fwd_sign` decidido por um actor cujo sinal é ruído

Duas tentativas, ambas erradas pelo mesmo motivo — uma única amostra cujo sinal
não significa nada:

1. Primeira: o desvio **lateral** do jogador local, contra `row0` — o eixo da
   direita, não o da frente. Eixo errado.
2. Segunda: a **profundidade** do jogador local. Eixo certo, mas o root do
   jogador local está quase em cima da câmara — medido a **0.14 m** numa
   corrida. A profundidade é um número pequeno cujo sinal é ruído de vírgula
   flutuante.

A segunda escolheu `+1` numa corrida em que uma votação sobre os outros seis
actores dizia `-1` com margem absoluta (`+1` punha **zero** no ecrã; `-1` punha
**seis**).

Correcção: votação sobre **todos** os actores promovidos, **excluindo o jogador
local**, contando quantos caem no ecrã sob cada sinal. Corre uma vez por frame
(`measure_forward_sign()`, chamada de `update_actors()`), não dentro do
`world_to_screen` — que corre por actor, e um sinal a mudar a meio de um frame
desenharia metade das caixas de cada maneira.

Resultado medido: **6 de 6** actores no ecrã com `-1`, **0 de 6** com `+1`.

### Bug 3 — o `poll_pool` corria uma única vez por sessão

O guard era:

```cpp
if (pool_cursor_ < pool_.size() && now - last_poll_ >= 120)
```

O `poll_pool` faz o `wrap` do cursor **dentro** do loop, por isso ao fim de uma
chamada o cursor fica **exactamente** em `pool_.size()` — e o guard fica falso
para sempre. Sintoma: `tracked 0`, `local player events 0`, e o próprio jogador
local (que se movia) nunca era visto a mexer.

A prova foi o contador de ciclos: **860** leituras em 30 s onde a aritmética
pedia ~215.000. Depois da correcção: **75.922**.

### Bug 4 — a janela nunca ficava visível (e a consola do lançador roubava o foco)

Dois problemas distintos que produziam "não aparece ESP nenhum" e "não consigo
atirar":

**(a)** `CreateWindowExW` é chamado **sem `WS_VISIBLE`**, logo a janela começa
escondida. Quem a mostrava era o gate de foreground, que foi removido por
oscilar. Resultado: ninguém mostrava a janela. Correcção: `end_frame` mostra-a
**uma vez**, depois do primeiro frame transparente ser apresentado. Mostrar
antes põe um backbuffer não-apresentado no ecrã, que numa janela layered é um
rectângulo **preto**.

**(b)** `bs_esp.exe` é `/SUBSYSTEM:WINDOWS` (verificado no header PE:
`Subsystem = 2`, não 3). Não tem consola própria. O output só chega ao ficheiro
porque o lançador lhe dá uma — e o `run_elevated.cmd` dava-lhe uma **visível**
(o `cmd` elevado), que ficava no ecrã, **tomava o foco**, e por isso o jogo não
recebia os botões do rato.

Correcção: `tools/run_hidden.ps1`. `-WindowStyle Hidden` tem de ser aplicado ao
**cmd elevado** (que é quem tem a consola), não ao PowerShell. E
`-Verb RunAs` **não pode** ser combinado com `-RedirectStandardOutput` — são
conjuntos de parâmetros mutuamente exclusivos, porque um processo lançado pelo
shell não tem handles padrão. Verificado experimentalmente.

### Bug 5 (relacionado) — `SetWindowPos(HWND_TOPMOST)` a cada frame

O `sync_to_target` chamava `SetWindowPos(HWND_TOPMOST, ...)` **duas vezes por
frame**, uma delas com o comentário "never let the game go behind us". O
`SWP_NOACTIVATE` evita a activação mas **não** o re-empilhamento — e re-empilhar
sobre o jogo 60×/s interfere com o raw input e com o `ClipCursor`, que é o que
o jogo usa para ler o rato.

`topmost` é uma **propriedade**, não uma acção por frame. Definida uma vez;
depois a janela só é **movida**, com `SWP_NOZORDER`.

---

## 0.3 ESTADO FINAL VERIFICADO (2026-10-17) — cadeia e posição fechadas

Tudo abaixo foi medido contra o processo em execução com o jogador **vivo e em
movimento**. Nenhum valor vem de fonte publicada.

```cpp
namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    namespace client {
        constexpr uint64_t IGameplay    = 0x58;
        constexpr uint64_t ClientPlayer = 0x58;
        constexpr uint64_t Camera       = 0xD88;
        constexpr uint64_t LocalPlayer  = 0xDD8;
    }
    namespace camera {
        constexpr uint64_t Position   = 0x7C;
        constexpr uint64_t Fov        = 0x454;
        constexpr uint64_t ViewMatrix = 0x41C;   // base 3x3 ortonormal, residual 0.00000
    }
    namespace entity {
        constexpr uint64_t ActorProps  = 0x88;   // medido; 0x48 e 0x3B8 estao errados
        constexpr uint64_t Component   = 0x18;
        constexpr uint64_t IEntity     = 0x40;   // intermitente, NAO usar
        constexpr uint64_t Position    = 0x7C;   // medido; 0x1C0 le zero em jogador vivo
        constexpr uint64_t Field3E0    = 0x3E0;  // pre-filtro fraco, ver 0.3c
    }
}
```

Cadeia usada pelo ESP:

```
base + 0x80D5980 -> ClientEngine
  +0x58          -> IGameplay
    +0x58        -> ClientPlayer
      +0xD88     -> Camera       pos +0x7C   fov +0x454   view +0x41C
      +0xDD8     -> LocalPlayer  pos +0x7C          <-- objecto do actor
        +0x18    -> ActorInstance
          +0x88  -> ActorProps
            +0x18-> Component
              +0x40 -> IEntity    (intermitente; ignorado)
```

### 0.3a `LocalPlayer + 0x7C` é a posição — prova

`pos_find` amostrou 300 vezes a 50 ms com o jogador em jogo. `LocalPlayer+0x7C`
foi o único campo que acompanhou a câmara em **300/300** amostras:

```
                     x: 16.12  24.16  32.00  39.31  50.61      cam: 16.09 24.26 32.10 39.41
LocalPlayer +0x7C    y: 192.25 190.93 191.39 191.83 193.97     cam: 193.75 192.43 192.89 193.33
                     z: 237.11 238.67 246.23 257.51 259.48     cam: 237.22 238.73 246.30 257.56
```

`x` e `z` coincidem ao milímetro com a câmara; `y` está exactamente 1,5 abaixo —
a altura do olho acima da raiz do actor. `+0x200` em diante é um array de `float3`
de ossos (`+0x208` ≈ raiz, `+0x214` ≈ 2,2 acima = cabeça).

### 0.3b `IEntity + 0x1C0` estava errado, e `+0x3E0` não é classe

`+0x1C0` leu `(0,0,0)` durante 90 s seguidos num jogador vivo. Tinha lido
`(-11.18, 0.15, -16.25)` num único sample anterior — coincidência suficiente para
construir cinco buscas erradas por cima.

O campo em `+0x3E0` **não é uma classe de entidade**. A sua distribuição entre
objectos com posição de mundo plausível é quase plana de 0 a 64: 647.071 leem 0,
106.052 leem 2, 12.645 leem 3. Isso é um contador ou um campo de bits.

### 0.3c Como os inimigos são encontrados sem entity list

Não há contentor. A assinatura é:

```
object + 0x3E0 == 2                        (pre-filtro fraco)
object + 0x7C  == posição de mundo finita, magnitude > 5
```

Isto reduz 9.794.745 ponteiros mapeados a 9.908 objectos. Ainda demasiados —
mas o discriminador final não é um número mágico:

> **jogadores mexem-se, cenário não.**

Amostrar os 9.908 duas vezes com 3 s de intervalo e comparar dá:

```
9.908 candidatos  ->  7 movers, incluindo o jogador local
```

As posições dos movers têm `y` entre 185 e 198 (a mesma altitude do local) e
espalham-se pelo mapa. É a drop de uma battle royale.

O `LocalPlayer` tem de estar sempre entre os movers — é a validação. Se um
filtro o deixa de fora, o filtro está errado.

### 0.3d Como o ESP usa isto

Não é preciso contentor. O ESP mantém um pool:

1. uma vez no arranque, a varredura de ponteiros produz os ~9.900 candidatos;
2. por frame, pollar ~1/4 do pool (cerca de 2.500 leituras de 4 bytes, ~5 ms);
3. quando um candidato se move mais de 0,5 unidades, é promovido a actor
   rastreado e a sua posição é lida de `+0x7C` a cada frame;
4. re-varrimento completo a cada 30 s para apanhar spawns novos.

Custo: ~5 ms/frame no poll, zero no desenho. Sem contentor para manter.

---

## 0.2 SEGUNDO DIAGNÓSTICO (2026-10-17) — o campo de posição também era falso

Cadeia medida e estável quando resolve:

```cpp
ClientEngine = 0x80D5980
  → IGameplay +0x58 → ClientPlayer +0x58
    → Camera       +0xD88   pos +0x7C   fov +0x454   viewMatrix +0x41C
    → LocalPlayer  +0xDD8
      → ActorInstance +0x18
        → ActorProps  +0x88   <-- NÃO 0x48 (ver 0.1b)
          → Component +0x18
            → IEntity +0x40    intermitente (ver 0.1c)
```

### 0.1b `ActorProps` é `0x88`, e `0x48` era sorte

`0x48` resolvia no lobby e dava um Component alinhado, com `EntityMask == 2` e
uma posição coerente com a câmara. Parecia certo. Em partida viva:

```
ActorProps + 0x18 -> 0x21996BD0101   <-- termina em 101
```

Todos os ponteiros reais desta heap terminam em `0` ou `8`. Um ponteiro
terminado em `101` não existe. O `+0x18` estava a devolver lixo, e o lixo
"parecia" um Component porque o campo seguinte tinha bits high a zero.

Com o scanner estrutural corrido contra um jogador **vivo e em movimento**, a
medição dá `+0x88`, e por esse caminho `EntityMask` volta a ser 2 e a posição
passa a coerente. `0x48` deixou de resolver.

### 0.1c `Component + 0x40 → IEntity` é intermitente

Resolve em alguns runs e falha em 60 tentativas consecutivas noutros. Não serve
como precondição. O que importa: `pos_find` deixou de o exigir e passa a
amostrar tudo o que resolver, deixando o diferencial temporal encontrar a
posição onde quer que ela esteja.

### 0.1d O campo de posição não é `0x1C0` — e `0x7C` também não

Dois captures de 15 s com a câmara a andar 67 unidades. Todos os `float3` de
todos os objectos da cadeia **congelados** durante a captura inteira:

```
LocalPlayer +0x0208
  camera   -13.20  -19.29  -17.79  -13.11  -12.64  -13.53  -15.56 ...
  value    -15.34  -15.34  -15.34  -15.34  -15.34  -15.34  -15.34 ...
```

Isto destrói o critério que todas as buscas anteriores usaram. "Estar perto da
câmara" não distingue uma posição viva de uma cópia congelada: um valor fixo
 pontua bem exactamente quando a câmara lhe passeia em volta, que foi o que
aconteceu duas vezes.

**Regra que daí resulta:** um campo de posição tem de **mover-se com o jogador**.
"Perto da câmara" é condição necessária mas insuficiente. `pos_find` agora
exige movimento máximo > 1 unidade antes de aceitar seja o que for, e aborta
com `[X] NOTHING MOVED` quando a câmara se mexe e o personagem não — o que
significa replay, câmara de espectador, ecrã de morte ou menu, não jogo.

### 0.1e Estado do que está resolvido

| | |
|---|---|
| `ClientEngine`, `IGameplay`, `ClientPlayer` | ✅ estáveis |
| `Camera` pos/fov/viewMatrix | ✅ estáveis, matriz ortonormal (residual 0.00000) |
| `LocalPlayer`, `ActorInstance` | ✅ estáveis |
| `ActorProps = 0x88` | ✅ medido contra jogador vivo |
| `Component = 0x18` | ✅ sob `0x88` |
| `IEntity = 0x40` | ⚠️ intermitente |
| `EntityMask = 0x3E0`, valor 2 = jogador | ✅ |
| **campo de posição** | ❌ **não identificado** |
| entity list | ❌ não necessária se o campo de posição aparecer |

O que bloqueia agora não é engenharia: é precisar de uma captura com o
personagem **a andar sob controlo do jogador** durante os 15 segundos.

---

## 0.1 DIAGNÓSTICO (2026-10-17, mais recente) — as 5 buscas falharam por falta de dados

A entity list nunca foi encontrada. A causa não era o método — era o estado do
jogo. Durante todas as tentativas o jogador local lia:

```
IEntity 0x219BD4ED850   mask 2   pos 0.00 0.00 0.00
```

O objecto existe e o `EntityMask` confirma que é um jogador, mas a posição é a
origem do mundo: **não há nada spawnado**. O mesmo endereço, no `struct_scan`,
lia `pos -17.18 -0.07 -7.63` a 1,4 unidades da câmara — ou seja, estava vivo
então e morreu/saiu depois.

Todos os testes de aceitação das cinco buscas exigiam "vários membros com
`mask == 2` e posições **distintas entre si**". Com todas as posições a zero esse
teste é insatisfazível por construção. Nenhuma das buscas podia passar, e as
quatro que "passaram" eram falsos positivos: pontos em `(0,0,0)` repetidos, que
qualquer teste de aceitação frouxo aceita numa heap com 5 milhões de ponteiros.

**Regra que daí resulta:** nunca procurar entidades com o jogo num estado em que
não existem entidades. `entity_scan.exe` tem um portão de preflight que aborta
com `[X] ABORT` neste estado, precisamente para não devolver uma lista vazia com
aparência de resposta válida.

### As cinco buscas e o que cada uma provou

| # | Abordagem | Resultado | O que eliminou |
|---|-----------|-----------|----------------|
| 1 | Lista ligada circular a partir de `0x8B78AA8` | não percorre | o nó não tem ponteiro seguinte |
| 2 | Array contíguo, 8-byte stride | "102 jogadores" | o mesmo `IEntity` 102× na origem |
| 3 | Array contíguo + filtro de magnitude | "18 jogadores, spread 2998" | posições `(-6851, -6775)` |
| 4 | `EntityHeapManager` / bucket table | 16.333 pares vector, zero jogadores | `0x7FCC540` é stale (`"llisionR"`) |
| 5 | Referência inversa ao jogador local | 36 referências, zero em array | o contentor não é plano a 8 bytes |

A tentativa 5 foi a informativa. Encontrou 36 referências ao jogador local
(22 ao `IEntity`, 11 ao `Component`, 2 ao `ActorInstance`, 1 ao `ActorProps`),
todas em heap, e **nenhuma** dentro de uma sequência de slots resolvíveis — a 8
bytes nem com strides livres de 0x10 a 0x60. As vizinhanças estão registadas em
`out/fr.txt`; contêm ponteiros para três objectos com `mask == 2` diferentes
(`0x219BD4F8D70` entre outros), o que confirma que existem outras instâncias de
jogador em memória.

### Saída: encontrar jogadores por assinatura, sem contentor

`entity_scan.cpp` elimina o contentor do problema. Três saltos estão verificados:

```
props + 0x18 -> Component
comp  + 0x40 -> IEntity
IEntity + 0x3E0 -> EntityMask == 2
IEntity + 0x1C0 -> position
```

Um jogador em memória tem portanto esta assinatura: existe `P` tal que
`*(P+0x18)` está mapeado e `*(*(P+0x18)+0x40)` é um `IEntity` com máscara 2.
É muito mais rara do que "um ponteiro" — o que matou as varrimentos anteriores.
Não há componente solto nesta assinatura: cada elemento tem de resolver por três
saltos verificados e trazer máscara de jogador.

Custo: ~1,4 GB lidos uma vez. Não corre por frame. Os `IEntity` resultantes são
cacheados e relidos por frame, descartando os que deixem de validar.

---

## 0. RESOLVIDO (2026-10-17) — cadeia verificada ao vivo

Tudo o que se segue foi **medido** contra o processo em execução, não deduzido.
Compilado em `chain_test.exe` e `struct_scan.exe`.

```cpp
namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;   // .data, confirmado
    namespace client {
        constexpr uint64_t IGameplay   = 0x58;
        constexpr uint64_t ClientPlayer= 0x58;
        constexpr uint64_t Camera      = 0xD88;
        constexpr uint64_t LocalPlayer = 0xDD8;
    }
    namespace camera {
        constexpr uint64_t Position   = 0x7C;      // (-16.50, 1.54, -6.78)
        constexpr uint64_t Fov        = 0x454;     // 112.02
        constexpr uint64_t ViewMatrix = 0x41C;     // base 4x3 ortonormal
    }
    // cadeia do jogador local, encontrada por medição (ver secção 0.1)
    constexpr uint64_t ActorProps = 0x48;
    constexpr uint64_t Component  = 0x18;
    constexpr uint64_t IEntity    = 0x40;
    constexpr uint64_t Position   = 0x1C0;         // (-17.18, -0.07, -7.63)
    constexpr uint64_t EntityMask = 0x3E0;         // 2 = jogador
}
```

Cadeia completa, com a leitura de cada valor confirmada:

```
base + 0x80D5980 -> ClientEngine
  +0x58          -> IGameplay
    +0x58        -> ClientPlayer
      +0xD88     -> Camera      pos (-16.50, 1.54, -6.78)   fov 112.02
      +0xDD8     -> LocalPlayer
        +0x18    -> ActorInstance
          +0x48  -> ActorProps
            +0x18-> ActorComponent
              +0x40 -> IEntity
                +0x1C0 -> position  (-17.18, -0.07, -7.63)
                +0x3E0 -> EntityMask = 2   (jogador)
```

A view matrix em `+0x41C` tem residual de ortogonalidade **0.00000** — é uma
base 3×3 verdadeira, com a translação de mundo na 4ª linha:

```
-0.0499  +0.0696  -0.9963
+0.0000  +0.9976  +0.0697
+0.9988  +0.0035  -0.0498
+5.9461  -0.3671 -16.8891
```

### 0.1 Como a cadeia do ActorProps foi encontrada

Os offsets publicados (`0x3B8`, `0x278`) não funcionam nesta build — devolvem
bytes ASCII. Em vez de chutar, `struct_scan.exe` varre todos os slots 4-byte do
`ActorInstance` × todos os saltos publicados, e aceita apenas o que satisfaz um
invariante espacial:

> a posição final tem de estar a menos de 10 unidades da câmara, cuja posição já
> conhecemos exactamente.

14 combinações passaram o teste e **todas chegam ao mesmo `IEntity`**
(`0x219BD4ED850`) com `EntityMask == 2`. Os saltos canónicos `+0x18` e `+0x40`
mantêm-se; o que muda é o primeiro elo: `0x48` nesta build.

O que prova que é a resposta certa e não ruído: `EntityMask` é exactamente 2
(o valor que a comunidade documenta para "jogador") e a posição está 1,4
unidades da câmara. Um falso positivo não satisfaz os dois.

### 0.2 O que ainda falta

**A entity list.** Isto é a única coisa que impede o ESP de desenhar caixas.
O global `EntityList = 0x8B78AA8` resolve para um endereço válido
(`0x21999F9856F0`), mas a forma publicada — lista ligada circular — não
percorre: o primeiro nó não tem ponteiro para o seguinte.

Sem a lista, só se conhece o jogador local. O ESP precisa de iterar inimigos.

### 0.3 Porquê que a análise estática não chegou aqui

Ver secção 2 (Source 2: falso alarme), secção 3 (RTTI cortado) e secção 6
(vftables cifradas). Resumo: o protector remove o ligame classe→código em disco
e em memória, e 21k nomes de classe sobrevivem sem endereço. A via que funciona
é medir o processo vivo, que é o que osChain de ferramentas acima fazem.


Data: 2026-10-17
Binário: `C:\Program Files (x86)\Steam\steamapps\common\BLOODSTRIKE\Engine\Binaries\Win64\BloodStrike.exe`
Build: `2026-10-11T16:56:26Z`

---

## 1. O que este build **é**

| | |
|---|---|
| Formato | PE32+ x64, GUI subsystem |
| Tamanho | 141.431.288 bytes (134,88 MB) |
| Image base | `0x140000000` (**RELOCS_STRIPPED** — base fixa, RVAs são absolutos) |
| Size of image | `0x94E9000` (148,9 MB) |
| Entry point | `0x424C4A4` |
| Proteções | `DYNAMIC_BASE`, `NX`, sem `GUARD_CF`, sem `NO_SEH` |

Secções:

```
name        rva        vsize      rawSize    rawPtr    flags
.text       0x1000     0x4e32aef  0x4e32c00  0x600     r-x
.rdata      0x4e34000  0x2801216  0x2801400  0x4e33200 r--
.data       0x7636000  0x189ee24  0xa99400   0x7634600 rw-
.pdata      0x8ed5000  0x389b8c   0x389c00   0x80cda00 r--
PyRuntim    0x925f000  0x4cfe0    0x4d000    0x8457600 rw-
AsyncioD    0x92ac000  0x68       0x200      0x84a4600 rw-
_RDATA      0x92ad000  0x20208    0x20400    0x84a4800 r--
.rsrc       0x92ce000  0x44270    0x44400    0x84c4c00 r--
.reloc      0x9313000  0x1d5704   0x1d5800   0x8509000 r--
```

> Atenção ao `rawPtr` do `.text`: é `0x600`, **não** `0x1000`. Qualquer cálculo
> manual `rva ↔ file offset` que assuma `raw == rva` erra por `0xE00` nas secções
> seguintes e produz COLs/vftables falsos. Foi exatamente esse o bug que fez a
> primeira versão do `rtti_dump.js` devolver zero classes.

## 2. CORRECÇÃO (2026-10-17): Source 2 era falso alarme

Uma leitura anterior deste ficheiro respondeu "tier0_s64.dll / vstdlib_s64.dll
= Valve Source 2". **Está errado.** Os dois ficheiros vivem em
`C:\Program Files (x86)\Steam\` — a raiz do Steam, numa pasta que não pertence ao
jogo — ao lado de `steamclient64.dll`, `SteamUI.dll`, `gameoverlayrenderer64.dll`
e todo o resto do runtime do cliente. São infrastructure do Steam, carregada
porque o jogo é launched pela Steam, e nada mais.

Versão资源 (também elas erradas antes):

```
tier0_s64.dll    887.960 bytes   company=Valve Corporation   product= tier0_s Dynamic Link Library
vstdlib_s64.dll  488.088 bytes   company=Valve Corporation   product=Steam
```

0,85 MB e 0,47 MB. Não é Source 2 — é a biblioteca de telemetria do cliente
Steam. A lista completa está em `out/module_paths.txt`.

Consequência: **não existe entity list do Source 2 para procurar.** A
abordagem da "fase 6c" foi abandonada por esta razão, não por thresholds
mal calibrados.

## 2b. A engine **não** é Unreal, nem Unity/IL2CPP

Determinado por RTTI e por strings:

```
.?AVScriptEngineProtocol@cocos2d@@
.?AVPythonEngine@CocosUI@Messiah@@
.?AVVCMatrixProp@Messiah@@
.?AV<lambda_2>@?BA@??GetCells@SpatialHashStreamingGrid@Messiah@@...
```

É o **Messiah** da NetEase — um fork de cocos2d-x. Confirmado por:

- assets em `.mpk` (Messiah Pack) em vez de `.pak` — 26 ficheiros, ~29 GB
- secções `PyRuntim` + `AsyncioD` → runtime Python embebido
- `Web/` com `libcef.dll` (login pela CEF)
- `Hydra_x64r.exe` (NetEase), `render.exe`, `UniCrashReporter.exe`

**Ausências confirmadas** (pesquisadas byte a byte em todo o ficheiro):

```
global-metadata.dat      -        il2cpp_init             -
libil2cpp                -        il2cpp_domain_get       -
GameAssembly             -        il2cpp_class_get_name   -
UnityPlayer              -        Assembly-CSharp         -
UnityEngine              -        IL2CPP                  -
```

Isto invalida a cadeia do `vex` (`UWorld → PersistentLevel → Actors → CameraCache`).
Não há `UWorld`, nem `GNames`/`FNamePool`, nem `GUObjectArray`.

### Consequência para o dump que foi fornecido

`bloodstrike-offsets.json` é do build **Android** (Unity IL2CPP, codebase
Facepunch Rust — `BasePlayer`, `clActiveItem`, `boneTransforms`, `TOD_Sky_C`,
`PlayerEyes`). É um codebase **completamente diferente** do PC.

Prova aritmética:

```
maior offset do dump          0xE7CCC60  (243.059.808)
BloodStrike.exe size of image 0x94E9000  (156.376.832)
0xE7CCC60 > 0x94E9000   ->  True
```

`il2cpp.get_handle`, `BaseNetworkable.typeinfo`, `main_camera.typeinfo` e
`BaseViewModel_C` estão todos **fora** do exe. Não servem para o PC.

## 3. Anti-cheat

| | |
|---|---|
| Driver em disco | nenhum `.sys` na instalação |
| Serviço registado | nenhum (`NEProtect` não está instalado nesta máquina) |
| Marcador no binário | `NEProtect` @ `0x6CFCB3D` |
| PE embebido | `0x6CF8C90` (x64, chars 0x22) — provável módulo do anti-cheat |
| Python | `PyRuntim`, `python3` @ `0x58C9758` — o HyperShield traz política em Python |

O que está instalado na máquina é o **Riot Vanguard** (`vgc`/`vgk`), deixado pelo
trabalho do Valorant. Não tem relação com o Blood Strike.

## 4. Estado estático do binário

Não está empacotado nem cifrado:

- entropia ~6,5 por todo o ficheiro (código normal); só 3 blocos de 4 MB > 7,5
- 209.363 runs ASCII imprimíveis = 5,45% do ficheiro
- strings de engine **ausentes** (`++UE5`, `Engine/Source/Runtime/Engine`,
  `/Script/Engine`) → binário *stripped*
- existem blobs de alta entropia dentro de `.rdata` (cifrados), que geram
  falsos positivos em qualquer scanner que procure ponteiros para `.text`

## 5. RTTI — o inventário de classes

O RTTI do MSVC sobreviveu, com uma codificação **não standard**:

```
struct TypeDescriptor { void* pVFTable; void* spare; char name[]; }   // 16 bytes de cabeçalho

struct RTTICompleteObjectLocator {    // campos em RVA ABSOLUTO, não self-relative
    DWORD signature;        // +0   = 1
    DWORD offset;           // +4
    DWORD cdOffset;         // +8
    int   pTypeDescriptor;  // +12  RVA absoluto
    int   pClassDescriptor; // +16  RVA absoluto
    int   pSelf;            // +20  RVA absoluto, igual ao próprio RVA
};
```

Verificado em `cocos2d::Node`: descriptor em `0x7CFDE48`, COL em `0x70AE848` com
`pSelf = 0x070AE848` (auto-consistente) e `pTypeDescriptor = 0x07CFDE48`.

Resultados (`tools/bloodstrike_rtti.json`):

```
type descriptors : 32.915
COLs válidos     : 19.572
vftables         : 17   (ver secção 6)
```

Namespaces com mais classes:

```
3354  Messiah                  479  Character                369  cocos2d
1378  Messiah::P8IObject        233  Character::GraphManager  207  Messiah::P8ActorComponent
 200  Messiah::P8SkeletonComponent        200 AnimationCore          235 physx
 143  Messiah::P8SoundComponent            102 mobile::server          64 async::logic
  86  spvtools::opt                       75 spine                   60 fairygui
```

`Character::` (1.211 tipos) é a camada de animação/câmara/cinematics do jogo —
candidatos diretos para ESP: `CameraCtrl`, `CameraFovCtrl`, `CameraPivotCtrl`,
`CameraForecastCollider`, `TransformExtracter`, `CollisionActorManager`,
`ObstacleAvoider`, `BlendTree`, `ActorProps`.

## 6. O que o protector bloqueia

**Não existe ponteiro para nenhum COL no ficheiro.** Varrimento byte a byte por
`uint32 == colRva` e por `uint64 == imageBase + colRva`: zero ocorrências.

Ou seja, o slot `[-1]` que normalmente precede cada vftable foi removido. As
19.572 classes têm nome e descriptor, mas **o ligame descriptor → vftable está
cortado no ficheiro**.

Consequência: qualquer dumper baseado em RTTI linkage (o método padrão para
Unity/UE) produz zero resultados neste build. Não é bug das ferramentas — é o
protector.

## 7. Caminho viável

O que **sobrevive** é o nome da classe. O que falta é o endereço. Logo o
resolver tem de ser **runtime**, não estático:

1. arrancar o jogo pelo launcher da Steam;
2. no processo vivo, enumerar objetos: qualquer qword no inicio de um objeto que
   resolva para dentro de `.text` é uma vtable, logo é uma instância;
3. para cada vtable encontrada, testar se o slot `[-1]` (restaurado em runtime,
   ou presente no processo mesmo tendo sido cortado no ficheiro) resolve para um
   COL válido → daí sai o `TypeDescriptor` → daí o nome das 32.915 classes;
4. localizar o singleton que guarda a lista de jogadores/actors;
5. `Character::CameraCtrl` / `TransformExtracter` para a matriz de câmara.

O passo 3 é a hypothesis testável que decide tudo: se o protector só corta o
link em disco, o inventário está completo e o resto é endereçamento. Se também
o remove em runtime, resta identification estrutural (o que é bem mais caro).

**Para testar isso é preciso o jogo a correr.** Não é possível aqui.

---

## Ferramentas em `tools/`

| Ficheiro | O que faz |
|---|---|
| `pe_scan.js` | triagem do PE: secções, markers de engine, censo de referências RIP-relative, assinaturas de accessor UE |
| `pack_probe.js` | responde "isto está empacotado/cifrado ou é só stripped?" — entropia por bloco, runs ASCII, PEs embebidos, marcadores de protector |
| `ue_find.js` | localiza globais UE (GWorld/GEngine/GUObjectArray) por análise de referências, com contexto de instrução. **Neste alvo não serve** (não é UE) — fica porque é útil noutros alvos UE |
| `rtti_dump.js` | inventário RTTI completo: 32.915 tipos, namespace, kind, TD/COL RVA. Escreve JSON |
| `bloodstrike_rtti.json` | o inventário |

Uso:

```
node tools\pack_probe.js "<caminho>\BloodStrike.exe"
node tools\pe_scan.js    "<caminho>\BloodStrike.exe" --refs 30
node tools\rtti_dump.js  "<caminho>\BloodStrike.exe" --json bloodstrike_rtti.json
node tools\rtti_dump.js  "<caminho>\BloodStrike.exe" --grep "^Character::"
```

Nota: usar `--max-old-space-size=6144` no `rtti_dump.js` (o buffer de 135 MB
mais 32.915 descriptors excede o heap default do Node).

## 8. História: o que veio do projeto anterior

Este repositório começou como um ESP para outro alvo (Valorant) e foi
redirecionado para o Blood Strike. Do projeto anterior **restou apenas o que era
genérico** — e o resto foi removido, não apenas desativado:

Aproveitado e ainda presente:

- `src/overlay.cpp` — janela transparente + swapchain DX11 + ImGui (genérico)
- `src/menu.cpp` — frontend do menu (genérico)
- `src/stealth.cpp` — hardening de imagem/processo/janela (genérico)
- `driver/vexdrv.sys` + `mapper/kernel_payload.c` — leitura de memória em kernel
- `src/sdk.hpp` — matemática (Vec3, Rotator, world-to-screen) — genérico

**Removido** (era específico do alvo antigo e não tem uso aqui): `main.cpp`,
`memory.cpp/hpp`, `mapper_client.cpp`, `config.cpp/hpp`, `calibrate.cpp/hpp`,
`entity.cpp/hpp`, `esp.cpp/hpp`, `renderer.cpp/hpp`, `globals.hpp`,
`offsets.hpp/json`. A leitura de memória em usermode do `bs_esp` é feita
diretamente com `OpenProcess` + `ReadProcessMemory` (ver `bs_esp.cpp`), não
através daquele backend.

O que **tem** de ser reescrito para o Blood Strike é a cadeia de memória:
`bs_esp.cpp` já a implementa para o engine Messiah (ver seções 0.x deste
documento). A nota histórica "o vex é ESP-only, sem aimbot" não vale para esta
árvore: existe aim assist em `src/aim.cpp`, desligado por omissão.
