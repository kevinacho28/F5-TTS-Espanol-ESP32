// ====================================================================================
#include <WiFi.h>
#include <FS.h>
#include "SD.h"
#include "SPI.h"
#include "driver/dac.h"
#include "time.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "freertos/semphr.h"

// --- (1) USER CONFIGURATION ---
const char* ssid = "UITEY EDUCA";
const char* password = "";
const char* server_host = "172.21.1.138";
const int   server_port = 5000;
const char* server_path = "/sintetizar";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -18000;
const int   daylightOffset_sec = 0;

// --- Screen Configuration ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

// --- Hardware Pins ---
const int DAC_PIN = 25;
const int MIC_PIN = 34;
const int SD_CS_PIN = 5;
const int RESET_BUTTON_PIN = 14;

// --- Audio Settings & Timeouts ---
const int SAMPLE_RATE = 16000;
const int PLAYBACK_PERIOD_US = 1000000 / SAMPLE_RATE;
const int RECORD_TIME_SECONDS = 5;
const long TOTAL_SAMPLES_TO_RECORD = (long)SAMPLE_RATE * RECORD_TIME_SECONDS;
const unsigned long RESPONSE_TIMEOUT_MS = 300000;
const unsigned long DOWNLOAD_STALL_TIMEOUT_MS = 300000;
const int MULTI_PRESS_TIMEOUT = 1000;
const unsigned long REPROGRAM_TIMEOUT_MS = 30000; 

// --- ESTRUCTURAS Y GLOBALES ---
enum SystemState { S_IDLE, S_PROGRAMMING_GENDER, S_PROGRAMMING_AGE, S_PREPARING, S_RECORDING, S_UPLOADING, S_WAITING_SERVER, S_DOWNLOADING, S_PLAYING, S_ERROR, S_SUCCESS, S_ASK_REPROGRAM };
SystemState currentState = S_IDLE;
int progressPercentage = 0;
String error_message = "";
String playbackFilename = "";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SemaphoreHandle_t stateMutex;
TaskHandle_t processingTaskHandle = NULL;

struct ProgrammableButton {
  const int pin;
  const char* original_filename;
  const char* original_display_name;
  const char* new_filename_path;
  String current_filename;
  String display_name;
  int press_count;
  unsigned long last_press_time;
};

ProgrammableButton buttons[] = {
  {27, "/baño.wav",   "1. Audio Bano",   "/cloned_1.wav", "/baño.wav",   "1. Audio Bano",   0, 0},
  {32, "/hambre.wav", "2. Audio Hambre", "/cloned_2.wav", "/hambre.wav", "2. Audio Hambre", 0, 0},
  {33, "/dolor.wav",  "3. Audio Dolor",  "/cloned_3.wav", "/dolor.wav",  "3. Audio Dolor",  0, 0},
  {12, "/sed.wav",    "4. Audio Sed",    "/cloned_4.wav", "/sed.wav",    "4. Audio Sed",    0, 0}
};
const int NUM_BUTTONS = sizeof(buttons) / sizeof(ProgrammableButton);

String userGender = "";
int userAge = 0;
int activeProcessingButtonIndex = -1;
unsigned long reprogramAskStartTime = 0;

// --- PROTOTIPOS ---
void updateDisplay();
void handleButtons();
bool recordAudio();
bool sendAudioAndProcessResponse();
void playbackTask(void *pvParameters);
void resetAllAssignments();
void connectToWiFi();
void syncTimeWithNTP();
void writeWavHeader(byte header[44], long totalSamples);
void processingTask(void *pvParameters);

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  for (int i = 0; i < NUM_BUTTONS; i++) pinMode(buttons[i].pin, INPUT_PULLUP);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  if (!SD.begin(SD_CS_PIN)) Serial.println("Fallo SD");
  Wire.begin();
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) { Serial.println(F("Fallo SSD1306")); }
  stateMutex = xSemaphoreCreateMutex();
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  dac_output_enable(DAC_CHANNEL_1);
  dac_output_voltage(DAC_CHANNEL_1, 128);
  connectToWiFi();
  syncTimeWithNTP();
  xTaskCreatePinnedToCore(processingTask, "ProcessingTask", 10000, NULL, 1, &processingTaskHandle, 1);
  delay(1000);
}

// --- LOOP (TAREA DE UI - NÚCLEO 0) ---
void loop() {
  handleButtons();
  updateDisplay();
  delay(20);
}

// --- GESTIÓN DE PANTALLA ---
void updateDisplay() {
  static SystemState lastDrawnState = (SystemState)-1;
  static int lastDrawnProgress = -1;
  static String lastDrawnErrorMessage = "";
  static int lastDrawnAge = 0;
  
  SystemState localCurrentState;
  int localProgress;
  String localErrorMessage;
  String localPlaybackFilename;

  if (xSemaphoreTake(stateMutex, (TickType_t)10) == pdTRUE) {
    localCurrentState = currentState;
    localProgress = progressPercentage;
    localErrorMessage = error_message;
    if (localCurrentState == S_PLAYING) localPlaybackFilename = playbackFilename;
    xSemaphoreGive(stateMutex);
  } else { return; }

  if (localCurrentState == lastDrawnState && localProgress == lastDrawnProgress && localErrorMessage == lastDrawnErrorMessage && (localCurrentState != S_PROGRAMMING_AGE || userAge == lastDrawnAge)) return;
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  
  switch (localCurrentState) {
      case S_IDLE:
        display.setCursor(0, 0);
        for (int i = 0; i < NUM_BUTTONS; i++) display.println(buttons[i].display_name);
        display.setCursor(0, 48);
        display.println("Info: 3 clicks para");
        display.println("      programar boton");
        break;
      case S_PROGRAMMING_GENDER:
        display.setTextSize(2); display.setCursor(0,0); display.println("Genero?");
        display.setTextSize(1); display.setCursor(0, 32); display.println("B1: Nino | B2: Nina");
        break;
      case S_PROGRAMMING_AGE:
        display.setTextSize(2); display.setCursor(0,0); display.println("Edad?");
        display.setTextSize(1); display.setCursor(0, 32); display.println("Edad: " + String(userAge) + " (Conf: B3)");
        lastDrawnAge = userAge;
        break;
      case S_PREPARING:
        display.setTextSize(2); display.setCursor(0, 24); display.println("Preparando...");
        break;
      case S_RECORDING:
      case S_DOWNLOADING:
        display.setTextSize(2); display.setCursor(0, 0);
        display.println(localCurrentState == S_RECORDING ? "Grabando" : "Descargando");
        display.drawRect(0, 30, 128, 12, SSD1306_WHITE);
        display.fillRect(2, 32, (int)(124 * (localProgress / 100.0)), 8, SSD1306_WHITE);
        display.setTextSize(1); display.setCursor(50, 48);
        display.print(localProgress); display.print("%");
        break;
      case S_UPLOADING:
        display.setTextSize(2); display.setCursor(0, 24); display.println("Enviando...");
        break;
      case S_WAITING_SERVER:
        display.setTextSize(2); display.setCursor(0, 16);
        display.println("Esperando"); display.setCursor(0, 36); display.println("Servidor...");
        break;
      case S_PLAYING:
        display.setTextSize(2); display.setCursor(0, 10); display.println("Reproduciendo");
        display.setTextSize(1); display.setCursor(0, 40);
        display.println(localPlaybackFilename.substring(1));
        break;
      case S_SUCCESS:
        display.setTextSize(2); display.setCursor(0, 24); display.println("Exito!");
        break;
      // --- NUEVO --- Pantalla para la nueva fase de pregunta
      case S_ASK_REPROGRAM:
        display.setTextSize(2); display.setCursor(0,0); display.println("Programar"); display.setCursor(0,18); display.println("otro?");
        display.setTextSize(1); display.setCursor(0, 40);
        display.println("SI: 3 clics en boton");
        display.println("NO: 1 clic en RESET");
        break;
      case S_ERROR:
        display.setTextSize(2); display.setCursor(0, 16); display.println("Error");
        display.setTextSize(1); display.setCursor(0, 40); display.println(localErrorMessage);
        break;
  }
  
  display.display();
  lastDrawnState = localCurrentState;
  lastDrawnProgress = localProgress;
  lastDrawnErrorMessage = localErrorMessage;
}

// --- LÓGICA DE BOTONES ---
void handleButtons() {
    SystemState localCurrentState;
    if (xSemaphoreTake(stateMutex, (TickType_t)10) == pdTRUE) { 
        localCurrentState = currentState; 
        xSemaphoreGive(stateMutex); 
    } else { 
        return; 
    }

    switch(localCurrentState) {
        case S_IDLE: {
            // Lógica para los botones programables (1-4)
            for (int i = 0; i < NUM_BUTTONS; i++) {
                if (digitalRead(buttons[i].pin) == LOW) {
                    delay(20);
                    if(digitalRead(buttons[i].pin) == LOW) {
                        buttons[i].press_count++;
                        buttons[i].last_press_time = millis();
                        while(digitalRead(buttons[i].pin) == LOW);
                    }
                }
                if (buttons[i].press_count > 0 && (millis() - buttons[i].last_press_time > MULTI_PRESS_TIMEOUT)) {
                    if (buttons[i].press_count == 1) {
                        Serial.printf("-> 1 click en Boton #%d: Reproduciendo %s\n", i + 1, buttons[i].current_filename.c_str());
                        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                            currentState = S_PLAYING;
                            playbackFilename = buttons[i].current_filename;
                            xSemaphoreGive(stateMutex);
                        }
                        updateDisplay(); 
                        xTaskCreatePinnedToCore(playbackTask, "playbackTask", 4096, NULL, 2, NULL, 1);
                    } else if (buttons[i].press_count == 3) {
                        Serial.printf("-> 3 clicks en Boton #%d: Iniciando flujo de programacion...\n", i + 1);
                        activeProcessingButtonIndex = i;
                        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_PROGRAMMING_GENDER; xSemaphoreGive(stateMutex); }
                    }
                    buttons[i].press_count = 0;
                }
            }
            
            // Lógica para el botón de reset (Triple Clic)
            static int reset_press_count = 0;
            static unsigned long last_reset_press_time = 0;

            if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                delay(20);
                if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                    reset_press_count++;
                    last_reset_press_time = millis();
                    Serial.printf("Reset button click #%d\n", reset_press_count);
                    while(digitalRead(RESET_BUTTON_PIN) == LOW); 
                }
            }

            if (reset_press_count > 0 && (millis() - last_reset_press_time > MULTI_PRESS_TIMEOUT)) {
                if (reset_press_count == 3) {
                    Serial.println("-> 3 clicks en RESET detectados! Restaurando audios originales...");
                    resetAllAssignments();
                } else {
                    Serial.printf("Reset cancelado. Se detectaron %d clicks, se requerian 3.\n", reset_press_count);
                }
                reset_press_count = 0; 
            }

        } break;

        case S_PROGRAMMING_GENDER: {
            if(digitalRead(buttons[0].pin) == LOW) { userGender = "nino"; Serial.println("Genero seleccionado: Nino"); if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_PROGRAMMING_AGE; xSemaphoreGive(stateMutex); } }
            if(digitalRead(buttons[1].pin) == LOW) { userGender = "nina"; Serial.println("Genero seleccionado: Nina"); if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_PROGRAMMING_AGE; xSemaphoreGive(stateMutex); } }
        } break;

        case S_PROGRAMMING_AGE: {
            if(digitalRead(buttons[0].pin) == LOW) { userAge--; if(userAge < 2) userAge = 2; while(digitalRead(buttons[0].pin) == LOW); }
            if(digitalRead(buttons[1].pin) == LOW) { userAge++; if(userAge > 12) userAge = 12; while(digitalRead(buttons[1].pin) == LOW); }
            if(digitalRead(buttons[2].pin) == LOW) { 
                Serial.printf("Edad confirmada: %d\n", userAge);
                if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_PREPARING; xSemaphoreGive(stateMutex); }
                updateDisplay();
                xTaskNotifyGive(processingTaskHandle);
            }
        } break;

        case S_ASK_REPROGRAM: {
            // Opción "NO": Presionar RESET una vez para volver a S_IDLE
            if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                delay(50); 
                if (digitalRead(RESET_BUTTON_PIN) == LOW) {
                    Serial.println("-> RESET presionado. Volviendo al modo IDLE.");
                    while(digitalRead(RESET_BUTTON_PIN) == LOW); 
                    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                        currentState = S_IDLE;
                        xSemaphoreGive(stateMutex);
                    }
                }
            }

            // Opción "SI": Presionar 3 veces otro botón
            for (int i = 0; i < NUM_BUTTONS; i++) {
                if (i == activeProcessingButtonIndex) continue; 

                if (digitalRead(buttons[i].pin) == LOW) {
                    delay(20);
                    if(digitalRead(buttons[i].pin) == LOW) {
                        buttons[i].press_count++;
                        buttons[i].last_press_time = millis();
                        while(digitalRead(buttons[i].pin) == LOW);
                    }
                }

                if (buttons[i].press_count > 0 && (millis() - buttons[i].last_press_time > MULTI_PRESS_TIMEOUT)) {
                    if (buttons[i].press_count == 3) {
                        Serial.printf("-> 3 clicks en Boton #%d: Reiniciando flujo para nuevo boton.\n", i + 1);
                        activeProcessingButtonIndex = i;
                        buttons[i].press_count = 0;
                        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                            currentState = S_PREPARING;
                            xSemaphoreGive(stateMutex);
                        }
                        updateDisplay(); 
                        xTaskNotifyGive(processingTaskHandle);
                    } else {
                       buttons[i].press_count = 0; 
                    }
                }
            }

            // Opción TIMEOUT
            unsigned long localStartTime;
            if (xSemaphoreTake(stateMutex, (TickType_t)10) == pdTRUE) {
              localStartTime = reprogramAskStartTime;
              xSemaphoreGive(stateMutex);
            } else { localStartTime = 0; }

            if (localStartTime != 0 && (millis() - localStartTime > REPROGRAM_TIMEOUT_MS)) {
                Serial.println("-> Timeout en pregunta. Volviendo a IDLE.");
                if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
                    currentState = S_IDLE;
                    xSemaphoreGive(stateMutex);
                }
            }
        } break;

        default: break;
    }
}

// --- TAREAs (NÚCLEO 1) ---
void processingTask(void *pvParameters) {
  Serial.println("Tarea de procesamiento iniciada en Nucleo 1.");
  for(;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Serial.printf("\n>>> Tarea de procesamiento iniciada para el boton #%d <<<\n", activeProcessingButtonIndex + 1);
    
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_RECORDING; xSemaphoreGive(stateMutex); }
    if (!recordAudio()) {
      Serial.println("Fallo la grabacion. Abortando tarea.");
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { error_message = "Fallo al grabar"; currentState = S_ERROR; xSemaphoreGive(stateMutex); }
      vTaskDelay(pdMS_TO_TICKS(3000));
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_IDLE; xSemaphoreGive(stateMutex); }
      continue;
    }
    
    if (!sendAudioAndProcessResponse()) {
      Serial.println("Fallo el envio/procesamiento. Abortando tarea.");
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_ERROR; xSemaphoreGive(stateMutex); }
      vTaskDelay(pdMS_TO_TICKS(3000));
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_IDLE; xSemaphoreGive(stateMutex); }
      continue;
    }
    
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
      currentState = S_SUCCESS;
      xSemaphoreGive(stateMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    
    Serial.println(">>> Tarea completada. Preguntando si desea reprogramar otro boton.");
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
      currentState = S_ASK_REPROGRAM;
      reprogramAskStartTime = millis(); 
      xSemaphoreGive(stateMutex);
    }
  }
}

bool recordAudio() {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { progressPercentage = 0; xSemaphoreGive(stateMutex); }
  String filename = "/recording_" + String(activeProcessingButtonIndex + 1) + ".wav";
  File audioFile = SD.open(filename, FILE_WRITE);
  if (!audioFile) { Serial.println("Fallo al abrir archivo para escribir!"); return false; }
  byte header[44];
  writeWavHeader(header, 0); 
  audioFile.write(header, 44);
  long samplesRecorded = 0;
  unsigned long lastSampleTime = 0;
  while(samplesRecorded < TOTAL_SAMPLES_TO_RECORD) {
    if (micros() - lastSampleTime >= PLAYBACK_PERIOD_US) {
      lastSampleTime = micros();
      static float last_input = 2048.0, last_output = 0.0;
      const float alpha = 0.8; 
      float raw_sample = analogRead(MIC_PIN);
      float filtered_output = alpha * (last_output + raw_sample - last_input);
      last_input = raw_sample; last_output = filtered_output;
      int final_sample = (int)((filtered_output + 2048.0) / 16.0);
      if (final_sample < 0) final_sample = 0;
      if (final_sample > 255) final_sample = 255;
      audioFile.write((uint8_t)final_sample);
      samplesRecorded++;
      if(samplesRecorded % (SAMPLE_RATE / 20) == 0) {
        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { progressPercentage = (int)(samplesRecorded * 100 / TOTAL_SAMPLES_TO_RECORD); xSemaphoreGive(stateMutex); }
      }
    }
  }
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { progressPercentage = 100; xSemaphoreGive(stateMutex); }
  writeWavHeader(header, samplesRecorded);
  audioFile.seek(0);
  audioFile.write(header, 44);
  audioFile.close();
  return true;
}

bool sendAudioAndProcessResponse() {
    auto setErrorState = [&](const char* msg) {
        if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { error_message = msg; xSemaphoreGive(stateMutex); }
    };

    int buttonIndex = activeProcessingButtonIndex;
    String recordingFilename = "/recording_" + String(buttonIndex + 1) + ".wav";
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_UPLOADING; xSemaphoreGive(stateMutex); }

    Serial.println("--- [Task] Iniciando conexión...");

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries++ < 20) vTaskDelay(pdMS_TO_TICKS(500));
        if (WiFi.status() != WL_CONNECTED) { setErrorState("Fallo WiFi"); return false; }
    }

    File fileToSend = SD.open(recordingFilename.c_str(), FILE_READ);
    if (!fileToSend) { setErrorState("Fallo SD"); return false; }

    WiFiClient client;
    if (!client.connect(server_host, server_port)) { fileToSend.close(); setErrorState("Fallo Servidor"); return false; }

    String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";

    String head = "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"audio\"; filename=\"" + recordingFilename.substring(1) + "\"\r\n";
    head += "Content-Type: audio/wav\r\n\r\n";

    String genderPart = "\r\n--" + boundary + "\r\n";
    genderPart += "Content-Disposition: form-data; name=\"genero\"\r\n\r\n";
    genderPart += userGender;

    String agePart = "\r\n--" + boundary + "\r\n";
    agePart += "Content-Disposition: form-data; name=\"edad\"\r\n\r\n";
    agePart += String(userAge);

    String tail = "\r\n--" + boundary + "--\r\n";

    // ---------- CALCULAR Content-Length ----------
    size_t contentLength = head.length() + fileToSend.size() + genderPart.length() + agePart.length() + tail.length();

    // ---------- ENVIAR CABECERAS ----------
    client.println("POST " + String(server_path) + " HTTP/1.1");
    client.println("Host: " + String(server_host));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println("Content-Length: " + String(contentLength));
    client.println("Connection: close");
    client.println(); // Fin de headers

    // ---------- ENVIAR CUERPO ----------
    client.print(head);

    uint8_t buffer[512];
    while (fileToSend.available()) {
        size_t bytesRead = fileToSend.read(buffer, sizeof(buffer));
        client.write(buffer, bytesRead);
    }
    fileToSend.close();

    client.print(genderPart);
    client.print(agePart);
    client.print(tail);

    Serial.println("--- [Task] Datos enviados. Esperando respuesta...");

    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        currentState = S_WAITING_SERVER;
        xSemaphoreGive(stateMutex);
    }

    // ---------- ESPERAR RESPUESTA ----------
    unsigned long timeout = millis();
    while (client.connected() && client.available() == 0) {
        if (millis() - timeout > RESPONSE_TIMEOUT_MS) {
            setErrorState("Timeout Servidor");
            client.stop();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    Serial.printf("--- [Task] Respuesta: %s\n", statusLine.c_str());
    if (!statusLine.startsWith("HTTP/1.1 200")) {
        client.stop(); setErrorState("Error Servidor"); return false;
    }

    while (client.available()) {
        String line = client.readStringUntil('\n');
        if (line == "\r" || line == "") break;
    }

    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        currentState = S_DOWNLOADING;
        progressPercentage = 50;
        xSemaphoreGive(stateMutex);
    }

    const char* savePath = buttons[buttonIndex].new_filename_path;
    if (SD.exists(savePath)) SD.remove(savePath);
    File responseFile = SD.open(savePath, FILE_WRITE);
    if (!responseFile) { setErrorState("Fallo al escribir"); return false; }

    long bytesReceived = 0;
    unsigned long stallTimeout = millis();

    while (client.connected() || client.available()) {
        if (client.available()) {
            int count = client.read(buffer, sizeof(buffer));
            if (count > 0) {
                responseFile.write(buffer, count);
                bytesReceived += count;
                stallTimeout = millis();
            }
        } else if (millis() - stallTimeout > DOWNLOAD_STALL_TIMEOUT_MS) {
            Serial.println("⚠ Timeout en descarga.");
            setErrorState("Descarga lenta");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    responseFile.close();
    client.stop();

    Serial.printf("Descarga completa. Bytes: %ld\n", bytesReceived);

    if (bytesReceived > 0) {
        buttons[buttonIndex].current_filename = buttons[buttonIndex].new_filename_path;
        buttons[buttonIndex].display_name = String(buttonIndex + 1) + ". Audio Personalizado";
        return true;
    } else {
        setErrorState("Respuesta vacía");
        return false;
    }
}


void playbackTask(void *pvParameters) {
  String localFilename;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    localFilename = playbackFilename;
    xSemaphoreGive(stateMutex);
  } else {
    vTaskDelete(NULL);
    return;
  }

  File fileToPlay = SD.open(localFilename);
  if (!fileToPlay) {
    Serial.println("Error: No se pudo abrir el archivo para reproduccion.");
    if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
      currentState = S_IDLE;
      xSemaphoreGive(stateMutex);
    }
    vTaskDelete(NULL);
    return;
  }

  const int bufferSize = 512; 
  uint8_t audioBuffer[bufferSize];
  size_t bytesRead;

  fileToPlay.seek(44); // Saltar la cabecera WAV

  unsigned long nextSampleTime = micros();

  while ((bytesRead = fileToPlay.read(audioBuffer, bufferSize)) > 0) {
    for (int i = 0; i < bytesRead; i++) {
      while (micros() < nextSampleTime) {
      }
      dac_output_voltage(DAC_CHANNEL_1, audioBuffer[i]);
      nextSampleTime += PLAYBACK_PERIOD_US;
    }
  }

  fileToPlay.close();

  // Silenciar el DAC y volver al estado IDLE
  dac_output_voltage(DAC_CHANNEL_1, 128); 
  vTaskDelay(pdMS_TO_TICKS(100));

  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    currentState = S_IDLE;
    xSemaphoreGive(stateMutex);
  }
  vTaskDelete(NULL); 
}

void resetAllAssignments() {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_PREPARING; xSemaphoreGive(stateMutex); }
  updateDisplay();
  vTaskDelay(pdMS_TO_TICKS(100));
  for (int i = 0; i < NUM_BUTTONS; i++) {
    buttons[i].current_filename = buttons[i].original_filename;
    buttons[i].display_name = buttons[i].original_display_name;
    if (SD.exists(buttons[i].new_filename_path)) {
      SD.remove(buttons[i].new_filename_path);
    }
  }
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_SUCCESS; xSemaphoreGive(stateMutex); }
  updateDisplay();
  vTaskDelay(pdMS_TO_TICKS(1500));
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) { currentState = S_IDLE; xSemaphoreGive(stateMutex); }
}

void connectToWiFi() { WiFi.begin(ssid, password); while (WiFi.status() != WL_CONNECTED) { delay(500); } }
void syncTimeWithNTP() { configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); }
void writeWavHeader(byte header[44], long totalSamples) {
  int bitsPerSample = 8;
  long dataSize = totalSamples * (bitsPerSample / 8);
  long chunkSize = dataSize + 44 - 8;
  long byteRate = (long)SAMPLE_RATE * (bitsPerSample / 8);
  long sampleRateLong = SAMPLE_RATE;
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F'; memcpy(&header[4], &chunkSize, 4);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E'; header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; header[20] = 1; header[21] = 0; header[22] = 1; header[23] = 0;
  memcpy(&header[24], &sampleRateLong, 4); memcpy(&header[28], &byteRate, 4);
  header[32] = bitsPerSample / 8; header[33] = 0; header[34] = bitsPerSample; header[35] = 0;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a'; memcpy(&header[40], &dataSize, 4);
}