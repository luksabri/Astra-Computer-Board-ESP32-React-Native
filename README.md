# 🚗 Astra Computer Board — ESP32 + React Native
![License](https://img.shields.io/badge/license-MIT-blue.svg)
![ESP32](https://img.shields.io/badge/Hardware-ESP32-green.svg)
![Bluetooth](https://img.shields.io/badge/Bluetooth-Classic%20(SPP)-0082fc.svg)

Uma solução open-source para criação de um **Computador de Bordo Inteligente** aplicado ao Chevrolet Astra 2.0 8V (com suporte adaptável a outros veículos injetados). 

O projeto consiste no firmware embarcado para **ESP32** (desenvolvido em C++/Arduino Framework) que realiza a leitura de sensores do veículo em tempo real via interrupções de hardware e ADC, processa métricas de consumo/velocidade/autonomia e transmite os dados via **Bluetooth Classic (SPP)** em formato JSON para um **Aplicativo Mobile em React Native**.
https://github.com/luksabri/Astra-Trip-Computer-React-Native-ESP32-

---

## 📌 Funcionalidades Principais

* **Velocidade em Tempo Real:** Leitura de pulsos do sensor VSS (*Vehicle Speed Sensor*).
* **Consumo Instantâneo:** Medição da largura de pulso da injeção eletrônica com suavização por média móvel.
  * Alternância automática de unidades: **L/h** (veículo parado/baixa velocidade) ou **km/L** (veículo em movimento).
* **Detecção de Cut-Off:** Identifica quando a ECU corta a injeção em desacelerações engrenadas acima de 5 km/h.
* **Nível de Tanque e Autonomia:** Leitura analógica da bóia de combustível (0-25V) com estimativa dinâmica de autonomia restante.
* **Múltiplos Odômetros Parciais (Trip A / Trip B):** Acumuladores independentes para distância, litros consumidos, tempo rodado e média de consumo ($\text{km/L}$).
* **Comunicação Bluetooth Bidirecional:**
  * Envio contínuo de dados via JSON estruturado para o aplicativo móvel.
  * Recepção de comandos remotos vindo do app (ex: `RESET_TRIP_A`, `RESET_TRIP_B`).
* **Arquitetura Multithread (FreeRTOS):** A tarefa de comunicação Bluetooth é processada em um núcleo dedicado (`Core 0`), mantendo o loop principal e as interrupções de hardware livres de travamentos no `Core 1`.

---

## 🏗️ Arquitetura do Sistema

```text
       [ Sensores do Veículo ]
     ( Bico Injetor | VSS | Bóia )
                  │
                  ▼
          [ Optocopladores ]
                  │
                  ▼
            ┌──────────┐
            │  ESP32   │  ── FreeRTOS (Core 0: BT / Core 1: Leitura)
            └──────────┘
                  │
          Bluetooth SPP (JSON)
                  │
                  ▼
    ┌──────────────────────────┐
    │  App Mobile React Native │  ── Exibição de Telemetria e Comandos
    └──────────────────────────┘
```
## ⚙️ Parâmetros de Calibração (Firmware)
**Se você estiver aplicando este projeto em outro veículo com especificações diferentes, ajuste as seguintes constantes no arquivo C++ do ESP32:**

```cpp
const int NUMERO_BICOS = 4;                 // Número de cilindros/bicos
const float VAZAO_BICO_ML_MS = 0.0035;      // Vazão individual (mL/ms)
const float PULSOS_VSS_POR_METRO = 8.0;     // Razão de pulsos/metro do VSS
const float CAPACIDADE_TANQUE_LITROS = 57.0;// Capacidade máxima do tanque
```

---

## 🧪 Testes de Bancada com 2º ESP32 (Simulador)

Para validar o funcionamento dos cálculos e a comunicação com o App sem precisar do carro, utilize o firmware [esp32-simulador-bancada.ino](file:///home/luk-dev/Vscode/Astra-Computer-Board-ESP32-React-Native/esp32-simulador-bancada.ino) gravado em um segundo ESP32.

### 🔌 Esquema de Ligação (Jumpers entre os dois ESP32s)

| ESP32 Simulador (`esp32-simulador-bancada.ino`) | ESP32 Principal (`esp32-astra-tripAeB.ino`) | Função / Sinal |
| :--- | :--- | :--- |
| **GND** | **GND** | **Terra comum (Obrigatório)** |
| **GPIO 19** (Out Bico) | **GPIO 33** (`PINO_BICO`) | Pulsos de Injeção (Ativo em LOW) |
| **GPIO 26** (Out VSS) | **GPIO 27** (`PINO_VSS`) | Frequência de Velocidade (Onda quadrada) |
| **GPIO 25** (Out DAC1 Bóia) | **GPIO 32** (`PINO_BOIA`) | Tensão Analógica do Tanque (0 a 3.3V) |

### 🎮 Controle Interativo via Monitor Serial do Simulador (115200 baud)

O simulador já inicializa em **Modo Automático de Viagem**, simulando aceleração, velocidade de cruzeiro, desaceleração com **Cut-off** e parada no semáforo.

Você também pode digitar comandos no Serial Monitor do ESP32 Simulador:
* `vel <km/h>`: Ajusta a velocidade (ex: `vel 80`).
* `consumo <valor>`: Ajusta o consumo em km/L ou L/h (ex: `consumo 12.0`).
* `tanque <0-100>`: Ajusta o nível do tanque em % (ex: `tanque 50`).
* `cutoff`: Força o estado de Cut-Off (velocidade > 0 e injeção zerada).
* `lenta`: Coloca o carro em marcha lenta parado (0 km/h, 1.1 L/h).
* `auto`: Liga ou desliga o ciclo automático de simulação.
* `status`: Exibe parâmetros atuais gerados pelo simulador.

