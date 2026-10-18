# Roteiro de teste — Blood Strike / bs_esp

Ladder de testes em degraus. **Não pule etapa** — cada uma isola uma camada de
falha, e ir direto para a partida ao vivo não dá informação útil quando algo dá
errado.

Alvo: **Blood Strike** (PC / Steam), engine **NetEase Messiah**.
Executável: `BloodStrike.exe`.

Antes de começar, o status honesto: veja `STATUS.md`. O overlay, o menu e a
varredura funcionam; **as caixas ainda não aparecem** e é o bloqueio conhecido.

---

## Degrau 0 — toolchain

| O quê | De onde | Tamanho |
|---|---|---|
| VS 2022 Build Tools | `aka.ms/vs/17/release/vs_BuildTools.exe` | ~4 MB |
| CMake | `github.com/Kitware/CMake/releases` (o `.msi`) | ~50 MB |
| Windows Driver Kit | instalador do VS, workload *Desktop development with C++* → aba *Individual Components* → **Windows Driver Kit** | ~2 GB |

No instalador, marque **"Desktop development with C++"** e, em componentes
individuais, o **Windows Driver Kit** (é ele que dá os headers de kernel para o
`vexdrv.sys` e o `kernel_payload.dll`).

Abra o **"x64 Native Tools Command Prompt for VS 2022"** e rode `build.bat` a
partir da raiz do repositório.

---

## Degrau 1 — build sem jogo

```
vex> build.bat
```

Esperado, em ordem:

```
=== 0/4  checking prerequisites ===
[ok] cmake 3.x
[ok] MSVC toolchain
[ok] Windows SDK / WDK headers
[ok] ImGui vendored in third_party\imgui (no git needed)
=== 1/4  configure ===
=== 2/4  bs_esp.exe ===
=== 3/4  kernel_payload.dll ===
=== 4/4  vexdrv.sys ===
...
  BUILD OK  --  run "...\vex\dist\bs_esp.exe"
```

O resultado vai para `dist\`. Se o `cl` não for reconhecido, você não está no
prompt do VS.

### Layout de saída

```
vex\dist\
├── bs_esp.exe               <- o overlay (é este que se roda)
├── kernel_payload.dll       <- shellcode do mapper (opcional)
├── vexdrv.sys               <- driver próprio (opcional)
└── iqvw64e.sys              <- seu BYOVD (opcional)
```

Os três últimos são **opcionais**: o `bs_esp` lê memória com
`OpenProcess` + `ReadProcessMemory` (ver `bs_esp.cpp`), então funciona sem
driver nenhum. A leitura em kernel é a via mais rápida/disfarçada, não a única.

---

## Degrau 2 — testes que não precisam do jogo

Estes são determinísticos e não exigem elevação. Se um falhar, o problema é de
lógica, não de jogo:

```
vex\dist> premul_test.exe
vex\dist> aim_test.exe
vex\dist> skeleton_filter_test.exe
```

Esperado: cada um imprime `0 failures`. O `premul_test` cobre o premultiply SSE2
do overlay; o `aim_test` cobre a seleção/suavização do aim assist; o
`skeleton_filter_test` cobre a decisão "é jogador / é cenário" contra os dois
objetos reais da corrida que não desenhou caixas.

`wnd_test.exe` também roda sem jogo e reproduz a criação da janela do overlay
isoladamente — se o overlay travar ao abrir, é aqui que se vê onde.

---

## Degrau 3 — overlay sem partida

O overlay precisa de elevação (acesso ao processo do jogo) e de um `cmd`
elevado. **Não** rode do PowerShell normal.

```
(abrir cmd como Administrador)
cd C:\caminho\para\vex\dist
.\bs_esp.exe --ulw
```

O jogo deve estar aberto e — importante — em **Janela sem bordas**
(*Borderless Windowed*), não em fullscreen exclusivo: overlay externo não
aparece por cima de fullscreen exclusivo.

Esperado: a janela do overlay sobe, o menu abre pela tecla designada, e o log
(escrito ao lado do exe) começa a crescer. Se o overlay subir mas não desenhar,
veja `wnd_test.exe`.

### Ler o log

O programa escreve o log ele mesmo, com flush por linha, num arquivo ao lado do
exe. Para seguir ao vivo, sem tomar foco do jogo:

```
vex> powershell -NoProfile -ExecutionPolicy Bypass -File tools\watch_log.ps1
```

Ou só as linhas de interesse:

```
Select-String -Path .\out\vex_log.txt -Pattern "\[pool\]|\[body\]|\[live\]|\[present\]"
```

---

## Degrau 4 — a partida (o teste decisivo)

Entre numa partida e **fique vivo e andando por pelo menos 60 segundos**. Não
morra, não volte ao menu — o `build_pool` da entrada da partida é justamente o
ponto sob suspeita (hipótese H1 em `STATUS.md`).

O log deve mostrar:

```
[pool] ... candidates kept
[body] ... pos-root ... head-pos ...
[live] fps .. tracked .. within 200m .. on-screen ..
```

**Como interpretar a linha `[live]`:**

| O campo | Significa | Se estiver errado |
|---|---|---|
| `tracked` | atores no pool | `1` = o pool nasceu de uma leitura ruim (H1) |
| `within 200m` | quantos estão a menos de 200 m | `0` com `tracked 1` = a posição do ator é inválida |
| `on-screen` | quantos projetam na tela | `0` = caixas não desenham |
| `nearest` | distância do mais próximo | `-1.0m` = nenhuma posição válida |

Se a linha `[body]` **nunca** aparecer, é a assinatura `+0x3E0 == 2` que está
errada (hipótese H2) — ver `STATUS.md`, seção do bloqueio principal, para o
passo seguinte em cada caso.

---

## Degrau 5 — validação visual

Com o bloqueio resolvido, o que se confere na partida:

1. caixas em cima dos jogadores (posição e escala);
2. filtro de time (`ignore teammates` deve sumir com metade);
3. radar e setas off-screen;
4. skeleton — hoje só root/head estão medidos, então é parcial;
5. performance: leia a linha `[present] cpu path: ...` (copy/map/premul/ulw).

Ajuste fino de aparência: no menu, `box scale`, `corner len`, `thickness`,
`dist_near/far`.

### Como saber se o filtro de time está certo

Ligue `ignore teammates`: se **metade** dos jogadores sumir, o time está certo
(5 aliados + 5 inimigos). Se **ninguém** sumir ou **todos** sumirem, o offset de
time lê a mesma coisa para todo mundo — está errado.

---

## Tabela de falhas

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `cl` não reconhecido | fora do prompt do VS | rode pelo "x64 Native Tools Command Prompt" |
| build falha em `kernel_payload` | headers de kernel ausentes | instale o **Windows Driver Kit** |
| overlay não aparece | jogo em fullscreen exclusivo | mude para Janela sem bordas |
| overlay aparece, mas nada desenha | caminho de apresentação | confirme `--ulw`; rode `wnd_test.exe` |
| log não cresce | processo sem permissão de escrita onde está | rode de `dist\`; o log cai ao lado do exe |
| `taskkill` dá acesso negado | `bs_esp` roda elevado | use `tools\kill_bs.ps1` elevado |
| caixas não aparecem | H1 ou H2 | ver `STATUS.md`, bloqueio principal |
| só root/head no skeleton | o resto dos ossos não está medido | ver `STATUS.md`, item 2 |
| nomes/vida não aparecem | offsets não medidos | interruptores desligados de propósito |
| mousa presa no overlay | janela tomando foco | verifique `WS_EX_NOACTIVATE`; veja `wnd_test` |

---

## Como reportar um problema

1. a saída **inteira** do log (`out\vex_log.txt`) da corrida;
2. as linhas `[pool]`, `[body]`, `[live]` e `[present]`;
3. o build (`build\` -> `CMakeCache.txt` diz o SDK/versão de MSVC).

Isso é suficiente para diagnosticar sem chute.
