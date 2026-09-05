/**
 * ===================================================================================
 *  🚗 SIMULADOR DE SENSORES DE BANCADA — ESP32 (Astra Computer Board)
 * ===================================================================================
 *  Este código roda em um SEGUNDO ESP32 e simula os sinais reais do Astra 2.0 8V:
 *   1. Bico Injetor (Pulsos ativos em nível LOW, simulando a ECU)
 *   2. Sensor VSS / Velocidade (Pulsos de onda quadrada calibrados a 14.921 pulsos/metro)
 *   3. Bóia de Combustível (Tensão analógica real de 0V a 3.3V via DAC interno)
 *
 *  Permite testar na bancada:
 *   - Velocidade instantânea (km/h)
 *   - Consumo instantâneo em L/h (parado) e km/L (em movimento)
 *   - Detecção de Cut-Off (desaceleração sem injeção)
 *   - Nível do tanque (%) e autonomia dinâmica
 *   - Odômetros parciais Trip A e Trip B
 *
 *  ----------------------------------------------------------------------------------
 *  🔌 ESQUEMA DE LIGAÇÃO ENTRE OS DOIS ESP32 (BANCADA):
 *  ----------------------------------------------------------------------------------
 *    ESP32 SIMULADOR (Este)          ESP32 PRINCIPAL (Computador de Bordo)
 *    ──────────────────────          ─────────────────────────────────────
 *    GND                     <=====> GND  (OBRIGATÓRIO: Terra comum)
 *    GPIO 19 (Sim Bico)      ------> GPIO 33 (PINO_BICO - Optocoplador/Entrada)
 *    GPIO 26 (Sim VSS)       ------> GPIO 27 (PINO_VSS - Velocidade)
 *    GPIO 25 (DAC1 Bóia)     ------> GPIO 32 (PINO_BOIA - Analógico)
 * ===================================================================================
 */

#include <Arduino.h>

// --- PINOS DE SAÍDA DO SIMULADOR ---
const int PIN_SIM_BICO = 19;  // Saída para o ESP32 Principal (Ativo em LOW)
const int PIN_SIM_VSS  = 26;  // Saída para o ESP32 Principal (Pulsos de Velocidade)
const int PIN_SIM_BOIA = 25;  // Saída DAC1 para o ESP32 Principal (Tensão Analógica)

// --- CONSTANTES DE CALIBRAÇÃO (Compatíveis com Astra / esp32-astra-tripAeB.ino) ---
const float PULSOS_VSS_POR_METRO = 16;  // Calibração: 14.921 pulsos/metro (~4.145 Hz por km/h)
const float FATOR_CONSUMO = 0.0504;         // Fator: ms_bico * 0.0504 = L/h

// --- ESTADOS DA SIMULAÇÃO ---
float simVelocidadeKmH = 0.0;         // Velocidade atual simulada (km/h)
float simConsumoDesejado = 1.2;       // km/L se > 5km/h, ou L/h se parado
float simTanquePorcento = 75.0;       // Nível do tanque (0 a 100%)
bool  simCutOff = false;              // Forçar Cut-Off

// Modo Automático de Viagem (Cenário pré-programado)
bool modoAutomatico = true;
unsigned long tempoInicioFaseAuto = 0;
int faseAutomatica = 0;

// Variáveis de controle de pulsos VSS
unsigned long tempoUltimoToggleVSS = 0;
unsigned long meioPeriodoVSS_micros = 0;
bool estadoPinoVSS = LOW;

// Variáveis de controle de injeção (Bico)
unsigned long tempoInicioCicloBico = 0;
unsigned long periodoCicloBicoMicros = 50000; // 20 Hz padrão (50ms por ciclo)
unsigned long larguraPulsoBicoMicros = 0;
bool estadoPinoBico = HIGH; // HIGH = bico fechado, LOW = bico injetando

// Controle de log serial
unsigned long tempoUltimoLog = 0;
const unsigned long INTERVALO_LOG = 1000; // 1 segundo

// Protótipos de funções
void atualizarParametrosFisicos();
void processarComandoSerial(String cmd);
void atualizarModoAutomatico();
void imprimirAjuda();

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n============================================================");
  Serial.println("🚗 ESP32 SIMULADOR DE SENSORES — ASTRA TRIP COMPUTER");
  Serial.println("============================================================");

  // Configuração dos pinos
  pinMode(PIN_SIM_BICO, OUTPUT);
  pinMode(PIN_SIM_VSS, OUTPUT);
  pinMode(PIN_SIM_BOIA, OUTPUT);

  // Estado inicial dos pinos
  digitalWrite(PIN_SIM_BICO, HIGH); // Bico desligado (HIGH)
  digitalWrite(PIN_SIM_VSS, LOW);   // VSS em nível baixo
  dacWrite(PIN_SIM_BOIA, 191);      // ~75% de combustível inicial

  atualizarParametrosFisicos();
  imprimirAjuda();

  Serial.println("\n[SIMULADOR] Inicializado com sucesso!");
  Serial.println("[SIMULADOR] VSS ajustado para: 14.921 pulsos/metro");
  Serial.println("[SIMULADOR] Modo automatico ativado por padrao (digite 'auto' para alternar para manual).\n");
}

void loop() {
  unsigned long agoraMicros = micros();
  unsigned long agoraMillis = millis();

  // 1. Processamento de comandos vindos do Monitor Serial
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    processarComandoSerial(cmd);
  }

  // 2. Atualização da simulação em modo automático (cenário de direção)
  if (modoAutomatico) {
    atualizarModoAutomatico();
  }

  // 3. GERAÇÃO DO SINAL VSS (Velocidade)
  if (simVelocidadeKmH > 0.5 && meioPeriodoVSS_micros > 0) {
    if (agoraMicros - tempoUltimoToggleVSS >= meioPeriodoVSS_micros) {
      tempoUltimoToggleVSS = agoraMicros;
      estadoPinoVSS = !estadoPinoVSS;
      digitalWrite(PIN_SIM_VSS, estadoPinoVSS);
    }
  } else {
    digitalWrite(PIN_SIM_VSS, LOW);
  }

  // 4. GERAÇÃO DO SINAL DO BICO INJETOR (Ativo em LOW)
  if (simCutOff || larguraPulsoBicoMicros == 0) {
    digitalWrite(PIN_SIM_BICO, HIGH);
  } else {
    unsigned long tempoDecorridoCiclo = agoraMicros - tempoInicioCicloBico;

    if (tempoDecorridoCiclo >= periodoCicloBicoMicros) {
      tempoInicioCicloBico = agoraMicros;
      digitalWrite(PIN_SIM_BICO, LOW); // Inicia pulso (ECU aterra o bico)
      estadoPinoBico = LOW;
    } else if (tempoDecorridoCiclo >= larguraPulsoBicoMicros && estadoPinoBico == LOW) {
      digitalWrite(PIN_SIM_BICO, HIGH); // Bico desliga
      estadoPinoBico = HIGH;
    }
  }

  // 5. LOG PERIÓDICO VIA SERIAL MONITOR
  if (agoraMillis - tempoUltimoLog >= INTERVALO_LOG) {
    tempoUltimoLog = agoraMillis;

    float freqVSS = (simVelocidadeKmH / 3.6) * PULSOS_VSS_POR_METRO;
    float msBicoPorSegundo = (larguraPulsoBicoMicros * (1000000.0 / periodoCicloBicoMicros)) / 1000.0;
    if (simCutOff) msBicoPorSegundo = 0.0;

    Serial.printf("[SIM] Vel: %5.1f km/h (%5.1f Hz) | Injecao: %5.1f ms/s | Tanque: %3.0f%% | %s | %s\n",
                  simVelocidadeKmH,
                  freqVSS,
                  msBicoPorSegundo,
                  simTanquePorcento,
                  simCutOff ? "CUT-OFF ATIVO" : (simVelocidadeKmH <= 5.0 ? "MARCHA LENTA" : "CRUZEIRO"),
                  modoAutomatico ? "[MODO AUTO]" : "[MODO MANUAL]");
  }
}

// --- ATUALIZAÇÃO DOS CÁLCULOS MATEMÁTICOS DOS SENSORES ---
void atualizarParametrosFisicos() {
  // 1. Calcula período do sinal VSS (onda quadrada)
  if (simVelocidadeKmH > 0.5) {
    float freqVSS = (simVelocidadeKmH / 3.6) * PULSOS_VSS_POR_METRO;
    meioPeriodoVSS_micros = (unsigned long)(500000.0 / freqVSS);
  } else {
    meioPeriodoVSS_micros = 0;
  }

  // 2. Frequência de repetição dos pulsos de injeção
  float freqBico = 20.0; // 20 Hz em marcha lenta
  if (simVelocidadeKmH > 5.0) {
    freqBico = 20.0 + (simVelocidadeKmH * 0.35); // Aumenta frequência conforme a velocidade simulada
  }
  periodoCicloBicoMicros = (unsigned long)(1000000.0 / freqBico);

  // 3. Calcula largura do pulso do bico injetor (tempo em nível LOW)
  if (simCutOff) {
    larguraPulsoBicoMicros = 0;
  } else {
    float msBicoTotalPorSegundo = 0.0;

    if (simVelocidadeKmH <= 5.0) {
      msBicoTotalPorSegundo = simConsumoDesejado / FATOR_CONSUMO;
    } else {
      if (simConsumoDesejado > 0.1) {
        msBicoTotalPorSegundo = simVelocidadeKmH / (simConsumoDesejado * FATOR_CONSUMO);
      } else {
        msBicoTotalPorSegundo = 0.0;
      }
    }

    float msPorPulso = msBicoTotalPorSegundo / freqBico;
    larguraPulsoBicoMicros = (unsigned long)(msPorPulso * 1000.0);

    // Limite de segurança: até 85% do ciclo
    if (larguraPulsoBicoMicros > (periodoCicloBicoMicros * 0.85)) {
      larguraPulsoBicoMicros = (unsigned long)(periodoCicloBicoMicros * 0.85);
    }
  }

  // 4. Atualiza tensão analógica da Bóia via DAC (0V a 3.3V)
  int valorDAC = (int)map((long)constrain(simTanquePorcento, 0, 100), 0, 100, 0, 255);
  dacWrite(PIN_SIM_BOIA, valorDAC);
}

// --- CICLO AUTOMÁTICO DE SIMULAÇÃO DE VIAGEM ---
void atualizarModoAutomatico() {
  unsigned long tempoDecorrido = millis() - tempoInicioFaseAuto;

  switch (faseAutomatica) {
    case 0: // Marcha lenta / Semáforo (6 segundos)
      simVelocidadeKmH = 0.0;
      simConsumoDesejado = 1.1;
      simCutOff = false;
      if (tempoDecorrido >= 6000) {
        faseAutomatica = 1;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Fase 1: Acelerando na cidade (0 -> 55 km/h)...");
      }
      break;

    case 1: // Aceleração urbana (8 segundos)
      simVelocidadeKmH = map(tempoDecorrido, 0, 8000, 0, 550) / 10.0;
      simConsumoDesejado = 6.2;
      simCutOff = false;
      if (tempoDecorrido >= 8000) {
        faseAutomatica = 2;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Fase 2: Velocidade de cruzeiro urbano (55 km/h)...");
      }
      break;

    case 2: // Cruzeiro Urbano (12 segundos)
      simVelocidadeKmH = 55.0;
      simConsumoDesejado = 10.8;
      simCutOff = false;
      if (tempoDecorrido >= 12000) {
        faseAutomatica = 3;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Fase 3: Entrando na Rodovia - Acelerando (55 -> 100 km/h)...");
      }
      break;

    case 3: // Aceleração Rodoviária (8 segundos)
      simVelocidadeKmH = map(tempoDecorrido, 0, 8000, 550, 1000) / 10.0;
      simConsumoDesejado = 7.5;
      simCutOff = false;
      if (tempoDecorrido >= 8000) {
        faseAutomatica = 4;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Fase 4: Cruzeiro na Rodovia (100 km/h, 13.5 km/L)...");
      }
      break;

    case 4: // Cruzeiro Rodovia (18 segundos)
      simVelocidadeKmH = 100.0;
      simConsumoDesejado = 13.5;
      simCutOff = false;
      simTanquePorcento = max(10.0f, simTanquePorcento - 0.05f);
      if (tempoDecorrido >= 18000) {
        faseAutomatica = 5;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Fase 5: TESTE DE CUT-OFF! Desacelerando com pe fora do acelerador...");
      }
      break;

    case 5: // Desaceleração com Cut-Off (8 segundos)
      simVelocidadeKmH = map(tempoDecorrido, 0, 8000, 1000, 400) / 10.0;
      simCutOff = true;
      if (tempoDecorrido >= 8000) {
        faseAutomatica = 6;
        tempoInicioFaseAuto = millis();
        simCutOff = false;
        Serial.println("\n>>> [AUTO] Fase 6: Freando ate parar no semaforo...");
      }
      break;

    case 6: // Parada total (5 segundos)
      simVelocidadeKmH = map(tempoDecorrido, 0, 5000, 400, 0) / 10.0;
      simConsumoDesejado = 9.0;
      simCutOff = false;
      if (tempoDecorrido >= 5000) {
        faseAutomatica = 0;
        tempoInicioFaseAuto = millis();
        Serial.println("\n>>> [AUTO] Carro parado. Reiniciando ciclo de viagem...\n");
      }
      break;
  }

  atualizarParametrosFisicos();
}

// --- PROCESSADOR DE COMANDOS DO MONITOR SERIAL ---
void processarComandoSerial(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  String cmdUpper = cmd;
  cmdUpper.toUpperCase();

  if (cmdUpper.startsWith("VEL")) {
    modoAutomatico = false;
    simCutOff = false;
    float v = cmd.substring(3).toFloat();
    simVelocidadeKmH = constrain(v, 0.0, 240.0);
    if (simVelocidadeKmH > 5.0 && simConsumoDesejado < 2.0) {
      simConsumoDesejado = 10.5;
    } else if (simVelocidadeKmH <= 5.0) {
      simConsumoDesejado = 1.1;
    }
    atualizarParametrosFisicos();
    Serial.printf("[CMD OK] Velocidade alterada para: %0.1f km/h (Modo Manual Ativado)\n", simVelocidadeKmH);
  }
  else if (cmdUpper.startsWith("CONSUMO")) {
    modoAutomatico = false;
    simCutOff = false;
    float c = cmd.substring(7).toFloat();
    simConsumoDesejado = max(0.1f, c);
    atualizarParametrosFisicos();
    Serial.printf("[CMD OK] Consumo configurado para: %0.2f %s\n",
                  simConsumoDesejado, (simVelocidadeKmH <= 5.0) ? "L/h" : "km/L");
  }
  else if (cmdUpper.startsWith("TANQUE")) {
    float t = cmd.substring(6).toFloat();
    simTanquePorcento = constrain(t, 0.0, 100.0);
    atualizarParametrosFisicos();
    Serial.printf("[CMD OK] Nivel do Tanque configurado para: %0.1f%%\n", simTanquePorcento);
  }
  else if (cmdUpper == "CUTOFF" || cmdUpper == "CUT-OFF") {
    modoAutomatico = false;
    simCutOff = true;
    if (simVelocidadeKmH <= 5.0) {
      simVelocidadeKmH = 80.0;
    }
    atualizarParametrosFisicos();
    Serial.println("[CMD OK] CUT-OFF ATIVADO! (Velocidade mantida e Injeção zerada)");
  }
  else if (cmdUpper == "LENTA") {
    modoAutomatico = false;
    simVelocidadeKmH = 0.0;
    simConsumoDesejado = 1.1;
    simCutOff = false;
    atualizarParametrosFisicos();
    Serial.println("[CMD OK] MARCHA LENTA ATIVADA! (0 km/h, 1.1 L/h)");
  }
  else if (cmdUpper == "AUTO") {
    modoAutomatico = !modoAutomatico;
    if (modoAutomatico) {
      faseAutomatica = 0;
      tempoInicioFaseAuto = millis();
      Serial.println("[CMD OK] MODO VIAGEM AUTOMÁTICA ATIVADO!");
    } else {
      Serial.println("[CMD OK] MODO AUTOMÁTICO DESATIVADO (Agora em Modo Manual).");
    }
  }
  else if (cmdUpper == "STATUS") {
    Serial.println("\n--- STATUS ATUAL DA SIMULAÇÃO ---");
    Serial.printf("Modo:        %s\n", modoAutomatico ? "AUTOMÁTICO" : "MANUAL");
    Serial.printf("Velocidade:  %0.1f km/h\n", simVelocidadeKmH);
    Serial.printf("Consumo:     %0.2f %s\n", simConsumoDesejado, (simVelocidadeKmH <= 5.0) ? "L/h" : "km/L");
    Serial.printf("Cut-Off:     %s\n", simCutOff ? "SIM (Ativo)" : "NÃO");
    Serial.printf("Tanque:      %0.1f%% (DAC: %ld)\n", simTanquePorcento, map((long)simTanquePorcento, 0, 100, 0, 255));
    Serial.println("---------------------------------\n");
  }
  else if (cmdUpper == "AJUDA" || cmdUpper == "HELP") {
    imprimirAjuda();
  }
  else {
    Serial.println("[CMD ERRO] Comando não reconhecido. Digite 'ajuda' para ver a lista.");
  }
}

// --- IMPRIME O MENU DE AJUDA NO SERIAL ---
void imprimirAjuda() {
  Serial.println("\n---------------- MENU DE COMANDOS (SERIAL) ----------------");
  Serial.println("  vel <valor>      -> Define velocidade (ex: vel 80)");
  Serial.println("  consumo <valor>  -> Define consumo (ex: consumo 12.5)");
  Serial.println("  tanque <0-100>   -> Define nivel do tanque (ex: tanque 50)");
  Serial.println("  cutoff           -> Simula Cut-off (injecao cortada)");
  Serial.println("  lenta            -> Simula Marcha Lenta (0 km/h, 1.1 L/h)");
  Serial.println("  auto             -> Alterna entre modo automatico e manual");
  Serial.println("  status           -> Exibe status completo atual");
  Serial.println("  ajuda            -> Mostra este menu");
  Serial.println("-----------------------------------------------------------\n");
}