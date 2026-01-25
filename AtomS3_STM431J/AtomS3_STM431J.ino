/*******************************************************************************
 * AtomS3-EnOcean-STM431J
 * 
 * @file    AtomS3_STM431J.ino
 * @brief   M5AtomS3とSTM431JモジュールでEnOceanデータを受信・表示
 * @date    2026-01-25
 * 
 * @description
 *   このプログラムは、M5AtomS3とEnOcean受信モジュールSTM431Jを使用して、
 *   EnOcean無線センサーからのデータを受信し、温度とデバイスIDをディスプレイに
 *   表示します。
 * 
 * @features
 *   - USB CDC-ACM経由でSTM431Jと通信
 *   - EnOcean ESP3プロトコル解析
 *   - EEP A5-02-05（温度センサー）対応
 *   - デバイスID（32bit）と温度（℃）をディスプレイ表示
 *   - CRC8チェックサムによるパケット検証
 * 
 * @hardware
 *   - M5AtomS3
 *   - USB400J EnOcean受信モジュール
 *   - STM431J EnOcean送信モジュール
 *   - MAX3232使用のシリアル接続（デバッグ用）
 * 
 * @pinout
 *   - GPIO5: UART RX (Serial2)
 *   - GPIO6: UART TX (Serial2)
 *   - USB: USB400J接続
 * 
 * @dependencies
 *   - M5AtomS3 Library
 *   - esp32-usb-serial (https://github.com/luc-github/esp32-usb-serial)
 * 
 * @communication
 *   - STM431J: 57600bps, 8N1, USB CDC-ACM
 *   - Serial2 (Debug): 115200bps, 8N1
 * 
 * @eep_support
 *   - A5-02-05: Temperature Sensor (0-40℃)
 * 
 * @operation
 *   - ボタンA: 画面クリア
 *   - 受信時: 自動的にデバイスID・温度を表示
 * 
 * @license
 *   MIT License (LICENSEファイルを参照)
 * 
 ******************************************************************************/

#include "M5AtomS3.h"

#include "esp32_usb_serial.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ESP_USB_SERIAL_BAUDRATE 57600
#define ESP_USB_SERIAL_DATA_BITS (8)
// 0: 1 stopbit, 1: 1.5 stopbits, 2: 2 stopbits
#define ESP_USB_SERIAL_PARITY (0)
// 0: None, 1: Odd, 2: Even, 3: Mark, 4: Space
#define ESP_USB_SERIAL_STOP_BITS (0)

#define ESP_USB_SERIAL_RX_BUFFER_SIZE 512
#define ESP_USB_SERIAL_TX_BUFFER_SIZE 128
#define ESP_USB_SERIAL_TASK_SIZE 4096
#define ESP_USB_SERIAL_TASK_CORE 1
#define ESP_USB_SERIAL_TASK_PRIORITY 10

SemaphoreHandle_t device_disconnected_sem;
std::unique_ptr<CdcAcmDevice> vcp;
bool isConnected = false;
bool usbReady = false;
TaskHandle_t xHandle;

// EnOcean受信バッファ
#define ENOCEAN_BUFFER_SIZE 256
uint8_t enoceanBuffer[ENOCEAN_BUFFER_SIZE];
size_t enoceanBufferIndex = 0;

// EnOceanパケット構造体
struct EnOceanPacket {
  uint8_t dataLength;
  uint8_t optionalLength;
  uint8_t packetType;
  uint8_t data[256];
  uint8_t optionalData[256];
  uint32_t senderId;
  float temperature;
  bool valid;
};

/**
 * @brief CRC8計算（EnOcean用）
 */
uint8_t calcCRC8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc = crc ^ data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x07;
      } else {
        crc = crc << 1;
      }
    }
  }
  return crc;
}

/**
 * @brief EnOceanパケット解析
 */
bool parseEnOceanPacket(const uint8_t *buffer, size_t len, EnOceanPacket &packet) {
  if (len < 6) return false;
  if (buffer[0] != 0x55) return false;  // 同期バイトチェック

  packet.dataLength = (buffer[1] << 8) | buffer[2];
  packet.optionalLength = buffer[3];
  packet.packetType = buffer[4];

  // パケット全体の長さチェック
  size_t totalLen = 6 + packet.dataLength + packet.optionalLength + 1;
  if (len < totalLen) return false;

  // ヘッダーCRCチェック
  uint8_t headerCRC = calcCRC8(&buffer[1], 4);
  if (headerCRC != buffer[5]) {
    Serial2.println("Header CRC error");
    return false;
  }

  // データ部分をコピー
  memcpy(packet.data, &buffer[6], packet.dataLength);
  memcpy(packet.optionalData, &buffer[6 + packet.dataLength], packet.optionalLength);

  // データCRCチェック
  uint8_t dataCRC = calcCRC8(&buffer[6], packet.dataLength + packet.optionalLength);
  if (dataCRC != buffer[6 + packet.dataLength + packet.optionalLength]) {
    Serial2.println("Data CRC error");
    return false;
  }

  packet.valid = true;

  // パケットタイプ 0x0A (RADIO_ERP1)の場合
  if (packet.packetType == 0x0A && packet.dataLength >= 10) {
    // EEP A5-02-05 (温度センサー)の解析
    uint8_t choice = packet.data[0];  // RORG (0x9B = 4BS)

    if (choice == 0x9B) {  // 4BS telegram
      // 送信元ID (最後の4バイト、Learn bitの前)
      packet.senderId = ((uint32_t)packet.data[packet.dataLength - 5] << 24) | ((uint32_t)packet.data[packet.dataLength - 4] << 16) | ((uint32_t)packet.data[packet.dataLength - 3] << 8) | ((uint32_t)packet.data[packet.dataLength - 2]);

      // A5-02-05: DB1に温度データ (0-250 = 0-40℃)
      uint8_t tempValue = packet.data[2];
      packet.temperature = (float)tempValue * 40.0 / 250.0;

      Serial2.printf("Device ID: %08X, Temp: %.1f C\n", packet.senderId, packet.temperature);
    }
  }

  return true;
}

/**
 * @brief Data received callback
 */
bool rx_callback(const uint8_t *data, size_t data_len, void *arg) {
  // デバッグ用：受信データを16進数で表示
  Serial2.print("RX: ");
  for (size_t i = 0; i < data_len; i++) {
    if (data[i] < 16) Serial2.print('0');
    Serial2.print(data[i], HEX);
    Serial2.print(' ');
  }
  Serial2.println();

  // バッファに追加
  for (size_t i = 0; i < data_len; i++) {
    if (enoceanBufferIndex >= ENOCEAN_BUFFER_SIZE) {
      enoceanBufferIndex = 0;  // オーバーフロー時はリセット
    }
    enoceanBuffer[enoceanBufferIndex++] = data[i];

    // 同期バイト検出
    if (data[i] == 0x55 && i == 0) {
      // 新しいパケットの開始
      enoceanBufferIndex = 1;
      enoceanBuffer[0] = 0x55;
    }
  }

  // パケット解析を試行
  if (enoceanBufferIndex >= 6) {
    EnOceanPacket packet;
    if (parseEnOceanPacket(enoceanBuffer, enoceanBufferIndex, packet)) {
      // 解析成功、ディスプレイを更新
      M5.Display.fillScreen(BLACK);
      M5.Display.setCursor(0, 0);
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(WHITE);

      M5.Display.println("EnOcean受信:");
      M5.Display.println();
      M5.Display.printf("ID: %08X\n", packet.senderId);
      M5.Display.println();
      M5.Display.setTextSize(2);
      M5.Display.printf("温度: %.1f C\n", packet.temperature);

      // バッファをクリア
      enoceanBufferIndex = 0;
    }
  }

  return true;
}

/**
 * @brief Device event callback
 *
 * Apart from handling device disconnection it doesn't do anything useful
 */
void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx) {
  switch (event->type) {
    case CDC_ACM_HOST_ERROR:
      Serial2.printf("CDC-ACM error has occurred, err_no = %d\n",
                     event->data.error);
      break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
      Serial2.println("Device suddenly disconnected");
      xSemaphoreGive(device_disconnected_sem);
      isConnected = false;
      break;
    case CDC_ACM_HOST_SERIAL_STATE:
      Serial2.printf("Serial state notif 0x%04X\n",
                     event->data.serial_state.val);
      break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
      Serial2.println("Network connection established");
      break;
    default:
      Serial2.println("Unknown event");
      break;
  }
}

void connectDevice() {
  if (!usbReady || isConnected) {
    return;
  }
  const cdc_acm_host_device_config_t dev_config = {
    .connection_timeout_ms = 5000,  // 5 seconds, enough time to plug the
                                    // device in or experiment with timeout
    .out_buffer_size = ESP_USB_SERIAL_TX_BUFFER_SIZE,
    .in_buffer_size = ESP_USB_SERIAL_RX_BUFFER_SIZE,
    .event_cb = handle_event,
    .data_cb = rx_callback,
    .user_arg = NULL,
  };
  cdc_acm_line_coding_t line_coding = {
    .dwDTERate = ESP_USB_SERIAL_BAUDRATE,
    .bCharFormat = ESP_USB_SERIAL_STOP_BITS,
    .bParityType = ESP_USB_SERIAL_PARITY,
    .bDataBits = ESP_USB_SERIAL_DATA_BITS,
  };
  // You don't need to know the device's VID and PID. Just plug in any device
  // and the VCP service will pick correct (already registered) driver for the
  // device
  Serial2.println("Opening any VCP device...");
  vcp = std::unique_ptr<CdcAcmDevice>(esp_usb::VCP::open(&dev_config));

  if (vcp == nullptr) {
    Serial2.println("Failed to open VCP device, retrying...");
    return;
  }

  vTaskDelay(10);

  Serial2.println("USB detected");

  if (vcp->line_coding_set(&line_coding) == ESP_OK) {
    Serial2.println("USB Connected");
    isConnected = true;
    uint16_t vid = esp_usb::getVID();
    uint16_t pid = esp_usb::getPID();
    Serial2.printf("USB device with VID: 0x%04X (%s), PID: 0x%04X (%s) found\n",
                   vid, esp_usb::getVIDString(), pid, esp_usb::getPIDString());
    xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
    vTaskDelay(10);

    vcp = nullptr;
  } else {
    Serial2.println("USB device not identified");
  }
}

// this task only handle connection
static void esp_usb_serial_connection_task(void *pvParameter) {
  (void)pvParameter;
  while (1) {
    /* Delay */
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!usbReady) {
      break;
    }
    connectDevice();
  }
  /* A task should NEVER return */
  vTaskDelete(NULL);
}

void setup() {
  auto cfg = M5.config();
  AtomS3.begin(cfg);

  // Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 5, 6);  // MAX3232側のシリアルの初期化
  delay(1000);
  Serial2.println("");
  Serial2.println("");

  // USB初期化
  if (ESP_OK != usb_serial_init()) {
    Serial2.println("Initialisation failed");
  } else {
    if (ESP_OK != usb_serial_create_task()) {
      Serial2.println("Task Creation failed");
    } else {
      Serial2.println("Success");
    }
    device_disconnected_sem = xSemaphoreCreateBinary();
    if (device_disconnected_sem == NULL) {
      Serial2.println("Semaphore creation failed");
      return;
    }
    BaseType_t res = xTaskCreatePinnedToCore(
      esp_usb_serial_connection_task, "esp_usb_serial_task",
      ESP_USB_SERIAL_TASK_SIZE, NULL, ESP_USB_SERIAL_TASK_PRIORITY, &xHandle,
      ESP_USB_SERIAL_TASK_CORE);
    if (res != pdPASS || !xHandle) {
      Serial2.println("Task creation failed");
      return;
    }
    Serial2.println("USB Serial Connection Task created successfully");
  }
  usbReady = true;

  // ディスプレイの初期化
  M5.Display.setRotation(1);
  M5.Display.setFont(&lgfxJapanGothic_12);
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("初期化完了");
  M5.Display.println("EnOcean待機中...");
}

void loop() {
  M5.update();

  if (M5.BtnA.isPressed()) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.setTextColor(WHITE);
    M5.Display.println("画面クリア");
    M5.Display.println("EnOcean待機中...");
  }
}
