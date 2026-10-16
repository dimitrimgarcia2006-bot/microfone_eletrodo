# Afinador eletrônico com ESP32-S3

Detector de notas musicais (Tema I). Um microfone de eletreto ligado ao ADC do
ESP32-S3 capta o som; o firmware calcula a FFT com a biblioteca
[esp-dsp](https://github.com/espressif/esp-dsp), encontra a frequência
fundamental e mostra a nota, de C3 a C5, com o desvio em cents.

```
Nota: A4 (La)  f0 = 440.3 Hz  ref = 440.00 Hz  +1 cents
```

Um script Python opcional mostra, em tempo real, o sinal do microfone e o
espectro calculado pelo ESP.

## Hardware

| Item       | Detalhe                                                                  |
|------------|--------------------------------------------------------------------------|
| Placa      | ESP32-S3 com conector USB nativo (USB Serial/JTAG)                       |
| Microfone  | eletreto de 2 terminais                                                  |
| Resistor   | polarização do eletreto, entre 3V3 e a saída do microfone (tipicamente 2,2 kΩ a 10 kΩ) |

```
  3V3 ───[ R ]───┬─────── GPIO4 (ADC1_CH3)
                 │
              saída
            [ microfone ]
             carcaça
                 │
  GND ───────────┘
```

- O sinal vai direto para o ADC, sem capacitor de acoplamento. A tensão de
  polarização (nível DC) é removida no software.
- Não há pré-amplificador: o sinal é pequeno, então a fonte sonora precisa
  estar perto do microfone.
- Use o conector **USB** (nativo) da placa, e não o **UART**. O visualizador
  só funciona pela USB nativa.

## Estrutura do repositório

```
main/
  main.c        laço principal: junta amostras em quadros e chama o detector
  config.h      parâmetros compartilhados (taxa, tamanho da FFT, faixa de notas)
  mic_adc.c/.h  leitura do microfone pelo ADC contínuo com DMA
  pitch.c/.h    janela de Hann, FFT e busca da frequência fundamental
  note.c/.h     conversão da frequência em nota e cents
  viz.c/.h      envio dos quadros para o visualizador pela USB
tools/
  visualizador.py   gráficos em tempo real do sinal e do espectro
  requirements.txt  dependências Python do visualizador
```

## Como funciona

```
 HARDWARE (não usa a CPU)                    │  SOFTWARE (task app_main)
                                             │
 microfone ─► ADC1 a 8 kHz ─► DMA ─► RAM     │
                               │             │
              a cada 256 amostras (32 ms)    │
                               ▼             │
                ISR do driver (interrupção)  │
                               │             │
                               ▼             │
                ringbuffer (2048 amostras) ──┼─► mic_adc_read()   bloqueia até ter dados
                                             │        │
                                             │        ▼
                                             │   frame[2048]       junta amostras, passo HOP
                                             │        │
                                             │        ▼
                                             │   pitch_process()   DC, Hann, FFT, f0
                                             │        │
                                             │        ▼
                                             │   note_from_freq()  f0 → nota + cents
                                             │        │
                                             │        ├─► printf (texto)
                                             │        └─► viz_send_frame() (visualizador)
```

### 1. Aquisição (`mic_adc.c`)

O ADC contínuo amostra a 8 kHz e o DMA grava as amostras na RAM sem usar a
CPU. A cada 256 amostras (32 ms), uma interrupção do driver coloca o bloco
num ringbuffer com folga de 256 ms. `mic_adc_read()` deixa a task
**bloqueada** até haver dados: não é polling, e a CPU fica livre enquanto
espera.

### 2. Segmentação (`main.c`)

As amostras são juntadas em quadros de `N_FFT` = 2048 amostras (256 ms). A
cada quadro processado, a segunda metade é reaproveitada no próximo (passo
`HOP` = 1024, 50% de sobreposição). Assim sai um resultado a cada 128 ms, e
nenhuma amostra fica só na borda da janela, onde a Hann quase a zera.

### 3. Detecção da fundamental (`pitch.c`)

1. **Remove a média**, que é a polarização do microfone.
2. **Aplica a janela de Hann**, que reduz o vazamento espectral.
3. **Calcula a FFT radix-2** (esp-dsp) e a magnitude `|X[k]|`. O bin `k`
   corresponde a `k × 3,906 Hz`.
4. **Acha o pico dominante** entre 129 Hz e 2,15 kHz. A busca vai até 4× o
   topo da faixa porque o pico mais forte pode ser um harmônico, não o
   fundamental.
5. **Testa o silêncio**: se a amplitude estimada do pico (`4·|X|/N`) for menor
   que 4 contagens do ADC, o quadro é silêncio.
6. **Testa os sub-harmônicos** `kp/4`, `kp/3` e `kp/2`, do mais grave para o
   mais agudo. É aceito o primeiro que estiver na faixa, for máximo local e
   tiver pelo menos 10% (−20 dB) da magnitude do pico dominante.
   *Exemplo:* num violão tocando A3 (220 Hz) com o 2º harmônico (440 Hz) mais
   forte, o pico dominante é 440 Hz, mas o teste de `kp/2` encontra o
   fundamental em 220 Hz.
7. **Refina a frequência** por interpolação parabólica no log da magnitude.
   É necessário porque, em C3, um bin equivale a 51 cents.
8. **Converte em nota**: `n = 49 + 12·log2(f0 / 440)` (tecla do piano, A4 = 49).
   Os cents são a diferença entre `n` real e `n` arredondado, vezes 100.

### 4. Visualizador (`viz.c` + `tools/visualizador.py`)

A cada quadro, o ESP monta um pacote de cerca de 8 KB: as 2048 amostras
brutas, os 1025 bins do espectro, a nota, o f0 e uma soma de verificação.
Esse pacote sai pela USB nativa, o que dá cerca de 64 KB/s. O formato está
documentado em `main/viz.h`.

- O ESP só envia enquanto o script manda um byte de "estou aqui" (`0x02`) a
  cada 0,5 s. Depois de 2 s sem esse byte, o envio para; assim o
  `idf.py monitor` mostra só texto quando o visualizador está fechado.
- O envio não trava o processamento: se o PC não der conta, o quadro inteiro
  é descartado.
- O texto do firmware (`printf`) passa pelo mesmo driver e aparece no
  terminal do script.

### Parâmetros principais

| Parâmetro          | Valor          | Arquivo     | O que faz                                  |
|--------------------|----------------|-------------|--------------------------------------------|
| `SAMPLE_RATE_HZ`   | 8000           | `config.h`  | taxa de amostragem                         |
| `N_FFT`            | 2048           | `config.h`  | tamanho do quadro e da FFT (potência de 2) |
| `HOP`              | `N_FFT / 2`    | `config.h`  | passo entre quadros (`N_FFT` = sem sobreposição) |
| `NOTE_F_MIN/MAX`   | C3 / C5        | `config.h`  | faixa de notas aceitas                     |
| `N_HARM_MAX`       | 4              | `pitch.c`   | maior harmônico testado                    |
| `SUBHARM_REL_DB`   | 20 dB          | `pitch.c`   | quanto o fundamental pode estar abaixo do pico |
| `MIN_AMP_CNT`      | 4 contagens    | `pitch.c`   | limiar de silêncio                         |
| `PRINT_EVERY`      | 2              | `main.c`    | imprime 1 a cada N quadros                 |
| `MIC_ADC_CHANNEL`  | `ADC_CHANNEL_3`| `mic_adc.c` | canal do ADC (GPIO4)                       |

Se mudar `MIN_AMP_CNT` ou a faixa de notas, atualize também as constantes no
topo de `tools/visualizador.py`. Elas são usadas só para desenhar as linhas de
referência; o tamanho do quadro o script lê do próprio pacote.

## Como usar

### Pré-requisitos

- **ESP-IDF v6.0** (testado com v6.0.1), instalado pelo EIM ou pela extensão
  do ESP-IDF no VS Code.
- **Python 3** para o visualizador (testado com 3.15).
- A esp-dsp é baixada sozinha pelo gerenciador de componentes
  (`main/idf_component.yml`).

### Compilar e gravar

Num terminal com o ambiente do ESP-IDF carregado, na raiz do repositório:

```
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

- `set-target` só é necessário na primeira vez: o `sdkconfig` não vai para o
  git e, sem ele, o IDF compila para o ESP32 comum.
- Troque `COM5` pela porta da sua placa. No Gerenciador de Dispositivos, em
  "Portas (COM e LPT)", a USB nativa aparece como "Dispositivo Serial USB".
  O número pode mudar quando a placa é reconectada.
- Para sair do monitor: `Ctrl+]`.

Saída esperada:

```
fs = 8000 Hz, N = 2048, df = 3.91 Hz, quadro = 256.0 ms, hop = 1024 (50% sobreposicao)
Faixa de notas: C3 (130.81 Hz) a C5 (523.25 Hz)
-- silencio
Nota: A4 (La)  f0 = 440.3 Hz  ref = 440.00 Hz  +1 cents
-- fora da faixa (pico em 1000.2 Hz)
```

### Visualizador em tempo real

Na primeira vez, crie o ambiente virtual na raiz do repositório (a pasta
`.venv/` já está no `.gitignore`):

```
python -m venv .venv
.venv\Scripts\pip install -r tools\requirements.txt
```

Depois, com a placa gravada e conectada pela USB nativa:

```
.venv\Scripts\python tools\visualizador.py
.venv\Scripts\python tools\visualizador.py --port COM5
```

Sem `--port`, o script procura a USB nativa do ESP32-S3 sozinho. A janela
mostra:

- **no topo**, a nota em letras grandes: verde se estiver a menos de 5 cents,
  amarela até 15 cents e vermelha acima disso;
- **sinal no tempo**: o quadro de 256 ms sem o nível DC (azul) e o mesmo sinal
  multiplicado pela janela de Hann (laranja), que é exatamente o que entra na
  FFT;
- **espectro**: a amplitude em dB, com a faixa C3..C5 em verde, a linha do
  limiar de silêncio e uma linha vermelha no f0 encontrado. Dá para dar zoom
  com a roda do mouse; o botão direito tem a opção "View All".

Cuidados:

- Feche o `idf.py monitor` antes de abrir o visualizador, porque só um
  programa por vez usa a porta.
- Feche o visualizador antes de gravar o firmware de novo, pelo mesmo motivo.
- Se a placa reiniciar com o script aberto, ele reconecta sozinho.

No Linux ou no macOS, troque `.venv\Scripts\` por `.venv/bin/` (não testado).

## Solução de problemas

| Sintoma | O que verificar |
|---------|-----------------|
| Só aparece `-- silencio` | O sinal está fraco: aproxime a fonte do microfone. No visualizador, confira se o DC está longe de 0 e de 4095 e se o sinal reage ao som. Se precisar, reduza `MIN_AMP_CNT`. |
| A nota sai uma oitava acima ou abaixo | O detector confundiu um harmônico com o fundamental. Ajuste `SUBHARM_REL_DB`: aumentar aceita fundamentais mais fracos, diminuir evita confundir ruído com o fundamental. |
| Visualizador: "Não achei a USB nativa do ESP32-S3" | O cabo está no conector UART ou a placa não está conectada. Também dá para indicar a porta com `--port`. |
| Visualizador fica em "sem conexão com COMx" | A porta está aberta em outro programa, normalmente o `idf.py monitor`. |
| `idf.py flash` diz que a porta está ocupada | O visualizador está aberto. |
| `dependencies.lock` muda sozinho | O gerenciador de componentes grava a versão do IDF usada. Se cada pessoa usa uma versão diferente (por exemplo 6.0.1 e 6.0.2), o arquivo muda a cada compilação; isso não causa problema. |
