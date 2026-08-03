# Astra-Computer-Board-ESP32-React-Native
# 🚗 Astra Computer Board — ESP32 + React Native

Uma solução open-source para criação de um **Computador de Bordo Inteligente** aplicado ao Chevrolet Astra 2.0 8V (com suporte adaptável a outros veículos injetados). 

O projeto consiste no firmware embarcado para **ESP32** (desenvolvido em C++/Arduino Framework) que realiza a leitura de sensores do veículo em tempo real via interrupções de hardware e ADC, processa métricas de consumo/velocidade/autonomia e transmite os dados via **Bluetooth Classic (SPP)** em formato JSON para um **Aplicativo Mobile em React Native**.

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