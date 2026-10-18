# vex — external ESP / overlay para Blood Strike (PC)

Cliente **externo** de overlay (ESP) para o jogo *Blood Strike* (PC / Steam,
engine **NetEase Messiah**). Nada é injetado no processo do jogo: a leitura de
memória acontece com `OpenProcess` + `ReadProcessMemory` (ou por um driver de
kernel opcional), e o desenho acontece numa **janela separada, transparente,
click-through**.

> **Aviso.** Isto é ferramenta de modificação de jogo. O anti-cheat do Blood
> Strike (NEProtect) pode detectar e banir. Use **conta descartável**. Do ponto
> de vista do Windows Defender / antivírus, este tipo de ferramenta se parece
> com malware — os avisos são esperados. Veja `STATUS.md` para o estado real do
> projeto (o que funciona e o que não).

---

## O que é

| Componente | Arquivo | O que faz |
|---|---|---|
| ESP externo | `bs_esp.exe` | overlay + menu + aim assist opcional (alvo Blood Strike) |
| Driver de leitura | `vexdrv.sys` | leitura/escrita de memória em kernel (opcional) |
| Shellcode | `kernel_payload.dll` | mapper em kernel, sem CRT (opcional) |
| Alvo | `BloodStrike.exe` | o jogo (engine NetEase Messiah) |

Existem também vários executáveis de **diagnóstico** (`bs_probe`, `chain_hunt`,
`chain_test`, `struct_scan`, `entity_list`, `players`, `pos_find`,
`entity_scan`, `find_refs`, `heap_mgr`, `bone_find`). Eles não fazem parte do
produto final: são as ferramentas que mediram os offsets contra o processo vivo.
Os testes (`premul_test`, `aim_test`, `skeleton_filter_test`) validam aritmética
sem precisar do jogo. Ver `BLOODSTRIKE_ANALYSIS.md`.

---

## Instalação

### 1. Toolchain

Você precisa do **Visual Studio 2022 Build Tools** com o **Windows Driver Kit**.

1. Baixe `https://aka.ms/vs/17/release/vs_BuildTools.exe` e execute.
2. Aba **"Cargas de trabalho"** → marque **"Desenvolvimento para desktop com C++"**.
3. Aba **"Componentes individuais"** → busque `Windows Driver Kit` → marque a
   categoria **"Windows Driver Kit"** inteira.
4. Instale (~10 GB, 20–60 min).

O **CMake** (>= 3.24) precisa estar no PATH — `https://cmake.org/download/`,
instalador Windows x64 `.msi`.

Confira abrindo o **"x64 Native Tools Command Prompt for VS 2022"** e digitando
`cl` — deve responder com a versão do compilador.

### 2. Build

No **"x64 Native Tools Command Prompt for VS 2022"**, a partir da raiz deste
repositório:

```
build.bat
```

Ele compila tudo e monta a pasta **`dist\`** ao lado do `build.bat`:

```
dist\
├── bs_esp.exe                <- o overlay (é este que se roda)
├── kernel_payload.dll
├── vexdrv.sys
└── (BYOVD, se você tiver um)
```

Para escolher outra pasta de saída: `build.bat C:\outra\pasta`.

### 3. Driver auxiliar (BYOVD) — opcional

O `bs_esp` lê memória diretamente com `OpenProcess` + `ReadProcessMemory`; não
precisa de driver nenhum para funcionar. O caminho em kernel é a via mais rápida
e mais discreta, não a única.

Para usá-lo, o mapper precisa de um driver assinado e vulnerável já presente no
sistema. Se você tiver um, coloque em `dist\` com um destes nomes exatos:

```
iqvw64e.sys   cpuz141.sys   ene.sys   rtcore.sys
```

O mapper tenta nessa ordem e usa o primeiro que achar.

### 4. Driver próprio `vexdrv.sys` — opcional

O `vexdrv.sys` deste projeto é feito para ser **mapeado manualmente** (via
`kernel_payload.dll`), não carregado por SCM — então, em geral, não precisa de
assinatura para esse caminho. Se você for carregá-lo pelo serviço do Windows,
ele precisa ser assinado:

```
cd sign
set VEXDRV_PFX_PASS=alguma-senha-local
sign_driver.bat
```

O script cria um certificado autoassinado, marca a máquina para confiar nele e
assina o `vexdrv.sys`. Depois: `bcdedit /set testsigning on` e reinicie.

> **Nunca** faça commit do `vexdrv.pfx` / `vexdrv.cer` que esse script gera. O
> `.pfx` contém uma **chave privada**. O `.gitignore` já os exclui.

---

## Como usar

1. Abra o jogo **em modo "Janela sem bordas" (Borderless Windowed)**. Overlay
   externo **não** aparece em fullscreen exclusivo.
2. Abra um **PowerShell/cmd como administrador** (o acesso ao processo exige
   elevação) e rode o executável do alvo a partir de `dist\`:

   ```
   cd dist
   .\bs_esp.exe --ulw
   ```

   (`--ulw` seleciona o caminho de apresentação `UpdateLayeredWindow`. É o
   caminho que funciona nesta máquina; veja `PENDENTE.txt`.)

3. O menu abre pela tecla designada. O ESP desenha caixas/health/skeleton sobre
   os jogadores detectados.

### Ferramentas de apoio

- `tools\launch.ps1` — sobe o `bs_esp` elevado e abre um terminal com o log.
- `tools\kill_bs.ps1` — mata o `bs_esp` (precisa de elevação).
- `tools\watch_log.ps1` — segue o log ao vivo.

---

## Estado do projeto

**Leia `STATUS.md`.** Ele diz, sem eufemismo, o que funciona hoje, o que não
funciona, o que está bloqueado e o que fazer para resolver cada item.

---

## Documentação técnica

| Arquivo | Conteúdo |
|---|---|
| `BLOODSTRIKE_ANALYSIS.md` | registo autoritativo do RE do alvo. Seção 0.6 é a mais recente |
| `STATUS.md` | estado honesto: o que funciona, o que não, como resolver |
| `PENDENTE.txt` | o bloqueio atual em detalhe técnico |
| `TESTING.md` | roteiro de testes em degraus |

---

## Aviso legal

Uso educacional / de pesquisa em engenharia de software. Modificar jogos viola os
termos de serviço e pode levar a banimento permanente. Você é responsável pelo
uso. Não use contra outros jogadores de forma predatória.
