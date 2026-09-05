#include <Arduino.h>
#include "BluetoothSerial.h"
#include <ArduinoJson.h>
#include <Preferences.h>

// Instâncias
BluetoothSerial SerialBT;
Preferences memoria;

// --- PINOS ---
const int PINO_BICO = 33;      // Entrada do optoacoplador do Bico Injetor
const int PINO_VSS = 27;       // Entrada do optoacoplador do VSS (Velocidade)
const int PINO_BOIA = 32;      // Entrada do módulo 0-25V da Bóia
const int PINO_POS_CHAVE = 25; // Entrada do optoacoplador do Pós-Chave (12V)

// --- CONFIGURAÇÕES E CONSTANTES DO ASTRA 2.0 8V ---
const int NUMERO_BICOS = 4;                 
const float VAZAO_BICO_ML_MS = 0.0035;      
const float PULSOS_VSS_POR_METRO = 16.95;  // Padrão real corrigido da linha GM
const float CAPACIDADE_TANQUE_LITROS = 52.0;

// --- CALIBRAÇÃO DO ADC DA BÓIA ---
const int ADC_TANQUE_CHEIO = 1000; 
const int ADC_TANQUE_VAZIO = 3100;

// --- CONFIGURAÇÃO DA MÉDIA MÓVEL (INSTANTÂNEO) ---
const int TAMANHO_AMOSTRAS = 3;             
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

// --- VARIÁVEIS COMPARTILHADAS E ESTADOS ---
volatile unsigned long tempoTotalInjecaoMicros = 0;
volatile unsigned long contadorPulsosVSS = 0;
volatile unsigned long tempoInicioPulsoBico = 0;
volatile unsigned long ultimoTempoVSS = 0; // Para o filtro anti-ruído do VSS

bool bluetoothConectado = false;
bool dadosSalvos = false; // Trava para impedir gravação repetida na memória

unsigned long tempoUltimoEnvioBT = 0;
const unsigned long INTERVALO_ENVIO_BT = 1000; 

// Dados processados (Globais)
float velocidadeKmH = 0.0;
float consumoCalculado = 0.0;
String unidadeConsumo = "L/h";
float nivelTanquePorcento = 0.0;
float litrosRestantesTanque = 0.0;
float autonomiaEstimadaKm = 0.0;
bool emCutOff = false;

// --- INTERRUPÇÕES ---

// Bico Injetor
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

// VSS (Velocidade) com Filtro Anti-Ruído (Debounce)
void IRAM_ATTR interpVSS() {
  unsigned long tempoAtual = micros();
  // Ignora pulsos que cheguem em menos de 500 microssegundos (bloqueia ruídos)
  //Para OFF o filtro
  if (tempoAtual - ultimoTempoVSS > 500) {
    contadorPulsosVSS++;
    ultimoTempoVSS = tempoAtual;
  }
}

// --- FUNÇÃO AUXILIAR DE MÉDIA MÓVEL ---
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
    // Zera também na memória flash
    memoria.putFloat("distA", 0.0);
    memoria.putFloat("litrosA", 0.0);
    memoria.putULong("tempoA", 0);
    Serial.println("[CMD BT] -> TRIP A Resetada!");
  } 
  else if (comando.indexOf("RESET_TRIP_B") >= 0) {
    tripB.distanciaMetros = 0;
    tripB.litrosConsumidos = 0;
    tripB.tempoSegundos = 0;
    // Zera também na memória flash
    memoria.putFloat("distB", 0.0);
    memoria.putFloat("litrosB", 0.0);
    memoria.putULong("tempoB", 0);
    Serial.println("[CMD BT] -> TRIP B Resetada!");
  }
}

// --- CALLBACK DE EVENTOS DO BLUETOOTH ---
void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    bluetoothConectado = true;
    Serial.println("[BT EVENT] CONECTADO!");
  } else if (event == ESP_SPP_CLOSE_EVT) {
    bluetoothConectado = false;
    Serial.println("[BT EVENT] DESCONECTADO!");
  }
}

// --- TAREFA DEDICADA DO BLUETOOTH (Core 0) ---
void TaskBluetooth(void *pvParameters) {
  for (;;) {
    if (SerialBT.available()) {
      String comandoRecebido = SerialBT.readStringUntil('\n');
      processarComandoBluetooth(comandoRecebido);
    }

    if (millis() - tempoUltimoEnvioBT >= INTERVALO_ENVIO_BT) {
      tempoUltimoEnvioBT = millis();

      if (SerialBT.hasClient()) {
        StaticJsonDocument<500> doc;

        JsonObject inst = doc.createNestedObject("instantaneo");
        inst["velocidade"] = velocidadeKmH;
        inst["consumo"] = consumoCalculado;
        inst["unidade"] = unidadeConsumo;
        inst["cutoff"] = emCutOff;

        JsonObject tanque = doc.createNestedObject("tanque");
        tanque["porcentagem"] = nivelTanquePorcento;
        tanque["litros_restantes"] = litrosRestantesTanque;
        tanque["autonomia_km"] = autonomiaEstimadaKm;

        JsonObject tA = doc.createNestedObject("trip_a");
        tA["km"] = tripA.distanciaMetros / 1000.0;
        tA["litros"] = tripA.litrosConsumidos;
        tA["media_kml"] = (tripA.litrosConsumidos > 0) ? ((tripA.distanciaMetros / 1000.0) / tripA.litrosConsumidos) : 0.0;
        tA["tempo_min"] = tripA.tempoSegundos / 60;

        JsonObject tB = doc.createNestedObject("trip_b");
        tB["km"] = tripB.distanciaMetros / 1000.0;
        tB["litros"] = tripB.litrosConsumidos;
        tB["media_kml"] = (tripB.litrosConsumidos > 0) ? ((tripB.distanciaMetros / 1000.0) / tripB.litrosConsumidos) : 0.0;
        tB["tempo_min"] = tripB.tempoSegundos / 60;

        String jsonSaida;
        serializeJson(doc, jsonSaida);
        SerialBT.println(jsonSaida);
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogSetAttenuation(ADC_11db);
  SerialBT.register_callback(btCallback);
  SerialBT.begin("Astra_Trip_Computer");

  pinMode(PINO_BICO, INPUT_PULLUP);
  pinMode(PINO_VSS, INPUT_PULLUP);
  pinMode(PINO_BOIA, INPUT);
  
  // Pino do pós-chave (HIGH = Desligado, LOW = Ligado)
  pinMode(PINO_POS_CHAVE, INPUT_PULLUP);

  // Inicia memória e recupera os dados salvos
  memoria.begin("viagem", false);
  tripA.distanciaMetros  = memoria.getFloat("distA", 0.0);
  tripA.litrosConsumidos = memoria.getFloat("litrosA", 0.0);
  tripA.tempoSegundos    = memoria.getULong("tempoA", 0);
  tripB.distanciaMetros  = memoria.getFloat("distB", 0.0);
  tripB.litrosConsumidos = memoria.getFloat("litrosB", 0.0);
  tripB.tempoSegundos    = memoria.getULong("tempoB", 0);
  Serial.println("[MEMÓRIA] Dados carregados!");

  attachInterrupt(digitalPinToInterrupt(PINO_BICO), interpBico, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PINO_VSS), interpVSS, RISING);

  xTaskCreatePinnedToCore(TaskBluetooth, "TaskBT", 4096, NULL, 1, NULL, 0);
}

void loop() {
  static unsigned long tempoUltimaMatematica = 0;
  
  // GATILHO DE SALVAMENTO DE ENERGIA (PÓS-CHAVE ESTABILIZADO)
  int estadoChave = digitalRead(PINO_POS_CHAVE);

  // Se o pino foi para HIGH (Chave Desligada) e ainda não salvou nesta sessão
  if (estadoChave == HIGH) {
    if (!dadosSalvos) { 
      Serial.println("[SISTEMA] Chave desligada. Gravando dados na flash...");
      memoria.putFloat("distA", tripA.distanciaMetros);
      memoria.putFloat("litrosA", tripA.litrosConsumidos);
      memoria.putULong("tempoA", tripA.tempoSegundos);
      memoria.putFloat("distB", tripB.distanciaMetros);
      memoria.putFloat("litrosB", tripB.litrosConsumidos);
      memoria.putULong("tempoB", tripB.tempoSegundos);
      
      dadosSalvos = true; // Trava permanentemente até o próximo boot/religamento real
      Serial.println("[MEMÓRIA] Gravação concluída!");
    }
  } 
  // Só destrava o salvamento se o pino retornar firmemente para LOW (Carro Ligado) 
  // e garantirmos que o sistema está rodando estabilizado.
  else if (estadoChave == LOW && dadosSalvos) {
    dadosSalvos = false; 
  }

  // MATEMÁTICA DE 1 SEGUNDO
  if (millis() - tempoUltimaMatematica >= 1000) {
    tempoUltimaMatematica = millis();

    // 1. Ler Nível do Tanque
    int leituraADC = analogRead(PINO_BOIA);
    float porcentoBruto = map(leituraADC, ADC_TANQUE_VAZIO, ADC_TANQUE_CHEIO, 0, 100);
    nivelTanquePorcento = constrain(porcentoBruto, 0.0, 100.0);
    litrosRestantesTanque = (nivelTanquePorcento / 100.0) * CAPACIDADE_TANQUE_LITROS;

    // 2. Extrair dados das interrupções de forma segura
    noInterrupts(); // Pausa interrupções
    unsigned long pulsosNaJanela = contadorPulsosVSS;
    contadorPulsosVSS = 0; 
    unsigned long microsBruto = tempoTotalInjecaoMicros;
    tempoTotalInjecaoMicros = 0; 
    interrupts();   // Retoma interrupções

    // 3. Velocidade
    float metrosNoSegundo = pulsosNaJanela / PULSOS_VSS_POR_METRO;
    velocidadeKmH = metrosNoSegundo * 3.6; 

    // 4. Bico e Consumo Instantâneo
    float microsSuavizado = calcularMediaBico((float)microsBruto);
    float msAbertoUnicoBico = microsSuavizado / 1000.0;
    float litrosConsumidosNoSegundo = (msAbertoUnicoBico * VAZAO_BICO_ML_MS * NUMERO_BICOS) / 1000.0;

    emCutOff = (velocidadeKmH > 5.0 && microsBruto == 0);

    if (velocidadeKmH <= 10.0) {
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

    // 5. Atualizar Trips (apenas se o carro estiver ligado e rodando)
    if (!dadosSalvos && (velocidadeKmH > 0 || litrosConsumidosNoSegundo > 0)) {
      tripA.distanciaMetros += metrosNoSegundo;
      tripA.litrosConsumidos += litrosConsumidosNoSegundo;
      tripA.tempoSegundos++;

      tripB.distanciaMetros += metrosNoSegundo;
      tripB.litrosConsumidos += litrosConsumidosNoSegundo;
      tripB.tempoSegundos++;
    }

    // 6. Autonomia
    float mediaParaAutonomia = 10.0; 
    if (tripA.litrosConsumidos > 0.1) { 
      mediaParaAutonomia = (tripA.distanciaMetros / 1000.0) / tripA.litrosConsumidos;
    }
    autonomiaEstimadaKm = litrosRestantesTanque * mediaParaAutonomia;

    // --- LOG DE MÁQUINA VIA SERIAL ---
    Serial.printf("[LOG] Vel: %0.1f km/h | Inst: %0.2f %s | Dist Trip A: %0.2f km | Salvo: %s\n", 
                  velocidadeKmH, consumoCalculado, unidadeConsumo.c_str(), (tripA.distanciaMetros / 1000.0), dadosSalvos ? "SIM" : "NAO");
  }
}