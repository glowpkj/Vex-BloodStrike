# STATUS — o que funciona e o que não

Última atualização: 2026-10-17.
Este documento é o estado **honesto** do projeto. Onde algo está quebrado, ele
diz que está quebrado. Onde algo é um palpite, ele diz que é palpite.

Alvo: **Blood Strike** (PC / Steam), engine **NetEase Messiah**.
Binário: `BloodStrike.exe`, build `2026-10-11T16:56:26Z`.

---

## Resumo em uma tabela

| Área | Estado | Detalhe |
|---|---|---|
| Compila | ✅ limpo | `build.bat` produz `dist\` |
| Overlay (janela transparente) | ✅ funciona | 44–75 fps, click-through |
| Menu / frontend | ✅ completo | abre mesmo no lobby |
| Varredura de memória fora da thread de desenho | ✅ funciona | varre 2227 MB sem travar o overlay |
| Detecção de entrada/saída de partida | ✅ funciona | |
| Blanking em morte | ✅ funciona | fov 54.6 → 40.0 → tela limpa |
| **Caixas sobre os jogadores** | ❌ **NÃO funciona** | **bloqueio principal** — ver abaixo |
| Skeleton | ⚠️ parcial | só root e head medidos; resto não |
| Nomes e barra de vida | ❌ não implementado | sem offset medido |
| Aim assist | ⚠️ estrutura pronta | desligado por omissão; não validado com o jogo a correr |
| Persistência de settings | ❌ não existe | restaurar padrão é só em memória |

---

## O bloqueio principal: as caixas não aparecem

Isto é o **único** problema que impede o ESP de ser útil. Está isolado e tem
duas hipóteses.

### O sintoma, com números

No log, numa partida real:

```
[pool] 3 candidates kept
[live] fps 59  tracked 1  within 200m 0  on-screen 0  nearest -1.0m
```

`tracked 1` e `within 200m 0` significam: o único ator rastreado tem distância
inválida. A linha `[body]` desta build **nunca imprimiu**.

### O que já está descartado (não gaste tempo aqui)

- **Não são os ossos.** Estão provados: `BoneRoot = 0x208`, `BoneHead = 0x214`,
  com a raiz a 11,6 cm dos pés e a cabeça a 2,16 m deles (`pos-root 0.116`,
  `head-pos 2.158`). Esses são exatamente os valores que a seção 0.3 do
  `BLOODSTRIKE_ANALYSIS.md` fixou como prova.
- **Não é o filtro de esqueleto.** Ele deixa passar o ator real.
- **Não é o pool que não cresce.** Isso *era* o problema e já foi corrigido
  (o sweep de fundo + merge, seção 0.6 do analysis).

### As duas hipóteses

**H1 — O `build_pool` inicial corre cedo demais.**
A varredura corre no instante `[match] entered a match`, quando a cadeia do
`local_` ainda pode não ter estabilizado (noutras corridas o `local_` leu
`(0,0,0)` exatamente nesse instante). Se o pool nasce de uma leitura ruim, nasce
ruim.

**H2 — A assinatura `+0x3E0 == 2` já não é a certa.**
Os números incomodam: 104958 passam o teste de 9,3 milhões (1,1%), e desses só
1023 têm posição legível (0,0097%). Isso é ruído, não uma assinatura.

### Como distinguir (o próximo passo)

1. Lance o jogo e o ESP (cmd elevado → `cd dist` → `.\bs_esp.exe --ulw`).
2. Entre numa partida e **fique vivo e andando por pelo menos 60 segundos**.
   Não morra, não volte ao menu.
3. Apanhe do log:

   ```
   Select-String -Path .\out\vex_log.txt -Pattern "\[pool\]|\[body\]|\[live\]"
   ```

**Como ler o resultado:**

- Se aparecer `[body] ... pos-root 0.116 ... head-pos 2.158`
  → **H1**. Os candidatos estão certos; é temporização. Correção: em
  `src\bs_esp.cpp` (~linha 2823, bloco `waiting_for_match_`), adiar o
  `build_pool()` até a cadeia estabilizar — exigir que o `local_` leia uma
  posição válida em **dois frames consecutivos** antes de varrer.
- Se o `[body]` **não** aparecer e o pool continuar com 1–3 candidatos
  → **H2**. A assinatura está errada. Precisa de uma ferramenta nova que meça o
  campo `+0x3E0` e ache o verdadeiro discriminador de "é um ator". O `bone_find`
  atual não serve (já correu e provou o que tinha a provar).

---

## O que fazer para resolver cada item

### 1. Caixas (prioridade máxima)
Ver a seção acima. Depende de uma corrida de diagnóstico (H1 vs H2).

### 2. Skeleton completo
Hoje só root/head estão medidos. Para desenhar o esqueleto inteiro são precisos
os offsets de **todos** os ossos. O `bone_find` tenta achar a array de ossos por
translação partilhada e devolveu **zero** em 4 objetos × 5 strides × 2240
offsets. Conclusão: os ossos **não** estão nos objetos da cadeia de posição
(`LocalPlayer`, `ActorInstance`, `ActorProps`, `Component`). A abordagem terá de
ser seguir ponteiros **para fora** dessa cadeia.

### 3. Nomes e barra de vida
Não há **nenhum** offset medido para nome nem para vida. Os interruptores
existem no menu, **desligados e rotulados** ("em desenvolvimento"), de propósito
— para o buraco ser visível em vez de silenciosamente ausente, e para nunca se
inventar um nome ou 100% de vida. Para implementar: medir os dois offsets no
processo vivo.

### 4. Aim assist
A matemática está pronta e testada (`aim_test`, 0 falhas); o passo que injeta
está implementado. **Não foi testado com o jogo a correr.** É o primeiro item a
validar quando o jogo abrir: seleção, suavização e ausência de movimento com o
recurso desligado.

> **Aviso:** o NEProtect (`0x6CFCB3D`) monitoriza o rato e enumera janelas. Aim
> assist com movimento de rato é **mais** detectável que escrever ângulo na
> memória. Use conta descartável.

### 5. Performance
O `[present]` varia entre 9,9 e 37,6 ms (pior caso = 27 fps de teto). O
premultiply está **descartado** como gargalo (mede 2,6 ms/frame). Suspeitos que
restam, por ordem: (1) o `Map` do DIB, (2) o `UpdateLayeredWindow` (`ulw`). O
log imprime `copy/map/premul/ulw` separados — leia a linha
`[present] cpu path: ...` para decidir. Se o `ulw` dominar, apresentar do staging
direto em vez de escrever num DIB.

---

## O que está PROVADO (não voltar a questionar)

```
LocalPlayer+0x7C     = posição (não 0x1C0)
ActorProps           = 0x88   (não 0x3B8, não 0x48)
Component+0x40       = IEntity, INTERMITENTE neste build
BoneRoot             = 0x208
BoneHead             = 0x214
matriz da câmara     = COLUMN-major, sign -1
pos.y - root.y       = 0.116  (pés à raiz do esqueleto)
head.y - pos.y       = 2.158  (cabeça a 2,16 m dos pés)
```

Cadeia usada pelo ESP:

```
base + 0x80D5980 -> ClientEngine
  +0x58          -> IGameplay
    +0x58        -> ClientPlayer
      +0xD88     -> Camera       pos +0x7C   fov +0x454   view +0x41C
      +0xDD8     -> LocalPlayer  pos +0x7C
        +0x18    -> ActorInstance
          +0x88  -> ActorProps
            +0x18-> Component
              +0x40 -> IEntity    (intermitente; ignorado)
```

---

## Armadilhas conhecidas (não cair de novo)

- `WS_EX_NOREDIRECTIONBITMAP` é incompatível com o caminho de apresentação deste
  projeto — trava o primeiro `Present`. O caminho é `LAYERED_CPU` (`--ulw`).
- A matriz da câmara é **COLUMN-major com sign -1** — é constante medida
  (`kForwardSign`/`kColMajor`). Não voltar a votar isto.
- `SetWindowPos(HWND_TOPMOST)` **por frame** perturba o raw input.
- `ShowWindow` é um evento de ativação — usar só na transição, não por frame.
- `AttachThreadInput` quebrou os botões do menu.
- `taskkill` sem elevação dá "Acesso negado" no `bs_esp`.
- O estande de tiro tem bots **parados** — dados de lá não valem para movimento.
- O `bone_find` precisa que você **ande em linha reta**. Jogador parado dá sinal
  fraco e o veredicto dele diz isso explicitamente.

---

## Anti-cheat (o que se sabe)

| | |
|---|---|
| Driver em disco | nenhum `.sys` na instalação do jogo |
| Serviço registado | nenhum (`NEProtect` não está instalado nesta máquina) |
| Marcador no binário | `NEProtect` @ `0x6CFCB3D` |
| PE embebido | `0x6CF8C90` (x64) — provável módulo do anti-cheat |
| Python | `PyRuntim` — o HyperShield traz política em Python |

O **Riot Vanguard** (`vgc`/`vgk`) não tem relação com o Blood Strike.
