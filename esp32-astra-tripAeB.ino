#include <Arduino.h>
#include "BluetoothSerial.h"
#include <ArduinoJson.h>

// Instância do Bluetooth
BluetoothSerial SerialBT;

// --- PINOS ---
const int PINO_BICO = 33;      // Entrada do optocoplador do Bico Injetor
const int PINO_VSS = 27;       // Entrada do optocoplador do VSS (Velocidade)
const int PINO_BOIA = 32;      // Entrada do módulo 0-25V da Bóia

// --- CONFIGURAÇÕES E CONSTANTES DO ASTRA 2.0 8V ---
const int NUMERO_BICOS = 4;                 // 4 cilindros / bicos
const float VAZAO_BICO_ML_MS = 0.0035;      // ~210 cc/min a 3.0 bar (0.0035 mL por ms)
const float PULSOS_VSS_POR_METRO = 8.0;     // ~8 pulsos por metro (8000 pulsos/km)
const float CAPACIDADE_TANQUE_LITROS = 57.0;// Capacidade total do tanque do Astra (57L)

// --- CONFIGURAÇÃO DA MÉDIA MÓVEL (INSTANTÂNEO) ---
const int TAMANHO_AMOSTRAS = 3;             // Média dos últimos 3 segundos
float historicoMicrosBico[TAMANHO_AMOSTRAS] = {0};
int indiceAmostra = 0;

// --- ESTRUTURA PARA CONTROLAR AS TRIPS ---
struct DadosTrip {
  float distanciaMetros = 0.0;
  float litrosConsumidos = 0.0;
  unsigned long tempoSegundos = 0;
};

DadosTrip tripA;
DadosTrip tripB;

// --- VARIÁVEIS COMPARTILHADAS (volatile para interrupções) ---
volatile unsigned long tempoTotalInjecaoMicros = 0;
volatile unsigned long contadorPulsosVSS = 0;
volatile unsigned long tempoInicioPulsoBico = 0;

// Estado da Conexão Bluetooth
bool bluetoothConectado = false;

// Controle de tempo de envio
unsigned long tempoUltimoEnvioBT = 0;
const unsigned long INTERVALO_ENVIO_BT = 1000; // Envia a cada 1 segundo

// Dados processados (Globais)
float velocidadeKmH = 0.0;
float consumoCalculado = 0.0;
String unidadeConsumo = "L/h";
float nivelTanquePorcento = 0.0;
float litrosRestantesTanque = 0.0;
float autonomiaEstimadaKm = 0.0;
bool emCutOff = false;

// --- INTERRUPÇÕES ---

// Interrupção do Bico Injetor (LOW = Optocoplador conduzindo/ECU aterrando bico)
void IRAM_ATTR interpBico() {
  if (digitalRead(PINO_BICO) == LOW) { 
    tempoInicioPulsoBico = micros();
  } else { 
    if (tempoInicioPulsoBico > 0) {
      tempoTotalInjecaoMicros += (micros() - tempoInicioPulsoBico);
      tempoInicioPulsoBico = 0;
    }
  }
}

// Interrupção do VSS (Velocidade)
void IRAM_ATTR interpVSS() {
  contadorPulsosVSS++;
}

// --- FUNÇÃO AUXILIAR DE MÉDIA MÓVEL (MANTIDA INTACTA) ---
float calcularMediaBico(float novoValor) {
  historicoMicrosBico[indiceAmostra] = novoValor;
  indiceAmostra = (indiceAmostra + 1) % TAMANHO_AMOSTRAS;
  
  float soma = 0;
  for (int i = 0; i < TAMANHO_AMOSTRAS; i++) {
    soma += historicoMicrosBico[i];
  }
  return soma / (float)TAMANHO_AMOSTRAS;
}

// --- FUNÇÃO PARA PROCESSAR COMANDOS RECEBIDOS DO REACT NATIVE ---
void processarComandoBluetooth(String comando) {
  comando.trim();
  
  if (comando.indexOf("RESET_TRIP_A") >= 0) {
    tripA.distanciaMetros = 0;
    tripA.litrosConsumidos = 0;
    tripA.tempoSegundos = 0;
    Serial.println("[CMD BT] -> TRIP A Resetada com sucesso!");
  } 
  else if (comando.indexOf("RESET_TRIP_B") >= 0) {
    tripB.distanciaMetros = 0;
    tripB.litrosConsumidos = 0;
    tripB.tempoSegundos = 0;
    Serial.println("[CMD BT] -> TRIP B Resetada com sucesso!");
  }
}

// --- CALLBACK DE EVENTOS DO BLUETOOTH ---
void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    bluetoothConectado = true;
    Serial.println("\n=========================================");
    Serial.println("[BT EVENT] Dispositivo Bluetooth CONECTADO!");
    Serial.println("=========================================\n");
  } else if (event == ESP_SPP_CLOSE_EVT) {
    bluetoothConectado = false;
    Serial.println("\n=========================================");
    Serial.println("[BT EVENT] Dispositivo Bluetooth DESCONECTADO!");
    Serial.println("=========================================\n");
  }
}

// --- TAREFA DEDICADA DO BLUETOOTH (Core 0) ---
void TaskBluetooth(void *pvParameters) {
  for (;;) {
    // 1. Ouve comandos vindos do App (React Native)
    if (SerialBT.available()) {
      String comandoRecebido = SerialBT.readStringUntil('\n');
      processarComandoBluetooth(comandoRecebido);
    }

    // 2. Envia o JSON estruturado a cada 1 segundo
    if (millis() - tempoUltimoEnvioBT >= INTERVALO_ENVIO_BT) {
      tempoUltimoEnvioBT = millis();

      if (SerialBT.hasClient()) {
        StaticJsonDocument<500> doc;

        // Módulo Instantâneo
        JsonObject inst = doc.createNestedObject("instantaneo");
        inst["velocidade"] = velocidadeKmH;
        inst["consumo"] = consumoCalculado;
        inst["unidade"] = unidadeConsumo;
        inst["cutoff"] = emCutOff;

        // Módulo Tanque
        JsonObject tanque = doc.createNestedObject("tanque");
        tanque["porcentagem"] = nivelTanquePorcento;
        tanque["litros_restantes"] = litrosRestantesTanque;
        tanque["autonomia_km"] = autonomiaEstimadaKm;

        // Módulo Trip A
        JsonObject tA = doc.createNestedObject("trip_a");
        tA["km"] = tripA.distanciaMetros / 1000.0;
        tA["litros"] = tripA.litrosConsumidos;
        tA["media_kml"] = (tripA.litrosConsumidos > 0) ? ((tripA.distanciaMetros / 1000.0) / tripA.litrosConsumidos) : 0.0;
        tA["tempo_min"] = tripA.tempoSegundos / 60;

        // Módulo Trip B
        JsonObject tB = doc.createNestedObject("trip_b");
        tB["km"] = tripB.distanciaMetros / 1000.0;
        tB["litros"] = tripB.litrosConsumidos;
        tB["media_kml"] = (tripB.litrosConsumidos > 0) ? ((tripB.distanciaMetros / 1000.0) / tripB.litrosConsumidos) : 0.0;
        tB["tempo_min"] = tripB.tempoSegundos / 60;

        String jsonSaida;
        serializeJson(doc, jsonSaida);
        SerialBT.println(jsonSaida);

        Serial.print("[BT SEND] -> JSON Enviado: ");
        Serial.println(jsonSaida);
      } else {
        Serial.println("[BT STATUS] Aguardando conexão do aplicativo no React Native...");
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- INICIALIZANDO COMPUTADOR DE BORDO ASTRA ESP32 ---");

  SerialBT.register_callback(btCallback);

  if (SerialBT.begin("Astra_Trip_Computer")) {
    Serial.println("[BT INIT] Bluetooth iniciado com sucesso! Nome: 'Astra_Trip_Computer'");
  } else {
    Serial.println("[BT ERROR] Falha ao iniciar o Bluetooth!");
  }

  pinMode(PINO_BICO, INPUT_PULLUP);
  pinMode(PINO_VSS, INPUT_PULLUP);
  pinMode(PINO_BOIA, INPUT);

  attachInterrupt(digitalPinToInterrupt(PINO_BICO), interpBico, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PINO_VSS), interpVSS, RISING);

  Serial.println("[GPIO INIT] Pinos e Interrupções configurados.");

  xTaskCreatePinnedToCore(
    TaskBluetooth,
    "TaskBT",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println("[SYSTEM] Sistema pronto! Iniciando leituras...\n");
}

void loop() {
  static unsigned long tempoUltimaMatematica = 0;
  if (millis() - tempoUltimaMatematica >= 1000) {
    tempoUltimaMatematica = millis();

    // 1. Ler Nível do Tanque (Bóia) e Calcular Litros Restantes
    int leituraADC = analogRead(PINO_BOIA);
    nivelTanquePorcento = map(leituraADC, 0, 4095, 0, 100);
    litrosRestantesTanque = (nivelTanquePorcento / 100.0) * CAPACIDADE_TANQUE_LITROS;

    // 2. Calcular Velocidade e Metros Rodados no Último Segundo
    unsigned long pulsosNaJanela = contadorPulsosVSS;
    contadorPulsosVSS = 0; 
    
    float metrosNoSegundo = pulsosNaJanela / PULSOS_VSS_POR_METRO;
    velocidadeKmH = metrosNoSegundo * 3.6; 

    // 3. Processar Tempo do Bico Injetor com Média Móvel (Função Preservada)
    unsigned long microsBruto = tempoTotalInjecaoMicros;
    tempoTotalInjecaoMicros = 0; 

    float microsSuavizado = calcularMediaBico((float)microsBruto);
    float msAbertoUnicoBico = microsSuavizado / 1000.0;
    
    // Litros consumidos no segundo atual
    float litrosConsumidosNoSegundo = (msAbertoUnicoBico * VAZAO_BICO_ML_MS * NUMERO_BICOS) / 1000.0;

    // Detecção de Cut-off (Carro em movimento acima de 5 km/h sem injeção de combustível)
    if (velocidadeKmH > 5.0 && microsBruto == 0) {
      emCutOff = true;
    } else {
      emCutOff = false;
    }

    // 4. Lógica do Consumo Instantâneo (Preservada)
    if (velocidadeKmH <= 5.0) {
      unidadeConsumo = "L/h";
      consumoCalculado = litrosConsumidosNoSegundo * 3600.0;
    } else {
      unidadeConsumo = "km/L";
      if (litrosConsumidosNoSegundo > 0) {
        float kmPercorridosNoSegundo = velocidadeKmH / 3600.0;
        consumoCalculado = kmPercorridosNoSegundo / litrosConsumidosNoSegundo;
      } else {
        consumoCalculado = 0.0; 
      }
    }

    // 5. ATUALIZAÇÃO DOS ACUMULADORES DAS TRIPS (Apenas em RAM)
    if (velocidadeKmH > 0 || litrosConsumidosNoSegundo > 0) {
      // Atualiza Trip A
      tripA.distanciaMetros += metrosNoSegundo;
      tripA.litrosConsumidos += litrosConsumidosNoSegundo;
      tripA.tempoSegundos++;

      // Atualiza Trip B
      tripB.distanciaMetros += metrosNoSegundo;
      tripB.litrosConsumidos += litrosConsumidosNoSegundo;
      tripB.tempoSegundos++;
    }

    // 6. CÁLCULO DA AUTONOMIA DINÂMICA
    // Usa a média da Trip A (ou assume 10 km/L padrão caso tenha acabado de resetar)
    float mediaParaAutonomia = 10.0; 
    if (tripA.litrosConsumidos > 0.1) { // Só calcula após consumir 100ml na viagem
      mediaParaAutonomia = (tripA.distanciaMetros / 1000.0) / tripA.litrosConsumidos;
    }
    autonomiaEstimadaKm = litrosRestantesTanque * mediaParaAutonomia;

    // --- LOG DE MÁQUINA VIA SERIAL ---
    Serial.println("--------------------------------------------------");
    Serial.printf("[LOG] Vel: %0.1f km/h | Inst: %0.2f %s | Trip A: %0.2f km (%0.2f km/L) | Autonomia: %0.0f km\n",
                  velocidadeKmH, consumoCalculado, unidadeConsumo.c_str(), (tripA.distanciaMetros / 1000.0), mediaParaAutonomia, autonomiaEstimadaKm);
  }
}