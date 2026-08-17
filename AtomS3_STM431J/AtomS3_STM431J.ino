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
 *   - M5Unified Library
 *   - EspUsbHost (https://github.com/tanakamasayuki/EspUsbHost)
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

#include "M5Unified.h"
#include "EspUsbHost.h"

#define USB_SERIAL_BAUDRATE 57600
#define USB_SERIAL_RX_BUFFER_SIZE 512

EspUsbHost usb;
EspUsbHostCdcSerial usbSerial(usb);
bool isConnected = false;

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

void displayEnOceanPacket(const EnOceanPacket &packet) {
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
}

/**
 * @brief EspUsbHostの受信リングから読み出したデータを処理
 */
void processReceivedData(const uint8_t *data, size_t data_len) {
  // デバッグ用：受信データを16進数で表示
  Serial2.print("RX: ");
  for (size_t i = 0; i < data_len; i++) {
    if (data[i] < 16) Serial2.print('0');
    Serial2.print(data[i], HEX);
    Serial2.print(' ');
  }
  Serial2.println();

  for (size_t i = 0; i < data_len; i++) {
    const uint8_t value = data[i];

    // 同期バイトを受信するまでは読み飛ばす
    if (enoceanBufferIndex == 0) {
      if (value != 0x55) {
        continue;
      }
      enoceanBuffer[enoceanBufferIndex++] = value;
      continue;
    }

    if (enoceanBufferIndex >= ENOCEAN_BUFFER_SIZE) {
      Serial2.println("EnOcean buffer overflow");
      enoceanBufferIndex = 0;
      continue;
    }
    enoceanBuffer[enoceanBufferIndex++] = value;

    if (enoceanBufferIndex < 6) {
      continue;
    }

    // ヘッダーが壊れている場合は早めに同期待ちへ戻る
    if (enoceanBufferIndex == 6 && calcCRC8(&enoceanBuffer[1], 4) != enoceanBuffer[5]) {
      Serial2.println("Header CRC error");
      enoceanBufferIndex = 0;
      continue;
    }

    const size_t packetLength = 7 +
                                (((size_t)enoceanBuffer[1] << 8) | enoceanBuffer[2]) +
                                enoceanBuffer[3];
    if (packetLength > ENOCEAN_BUFFER_SIZE) {
      Serial2.println("EnOcean packet too large");
      enoceanBufferIndex = 0;
      continue;
    }

    if (enoceanBufferIndex == packetLength) {
      EnOceanPacket packet = {};
      if (parseEnOceanPacket(enoceanBuffer, enoceanBufferIndex, packet)) {
        displayEnOceanPacket(packet);
      }
      enoceanBufferIndex = 0;
    }
  }
}

void handleUsbSerialInput() {
  uint8_t data[64];

  while (usbSerial.available() > 0) {
    size_t dataLength = 0;
    while (dataLength < sizeof(data) && usbSerial.available() > 0) {
      const int value = usbSerial.read();
      if (value < 0) {
        break;
      }
      data[dataLength++] = (uint8_t)value;
    }

    if (dataLength == 0) {
      break;
    }
    processReceivedData(data, dataLength);
  }
}

void updateUsbConnectionState() {
  const bool connected = usbSerial.connected();
  if (connected == isConnected) {
    return;
  }

  isConnected = connected;
  Serial2.println(isConnected ? "USB Serial Connected" : "USB Serial Disconnected");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  // Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 5, 6);  // MAX3232側のシリアルの初期化
  delay(1000);
  Serial2.println("");
  Serial2.println("");

  // USB初期化
  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    Serial2.print("USB device connected: ");
    espUsbHostPrint(device, Serial2);
  });
  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    Serial2.print("USB device disconnected: ");
    espUsbHostPrint(device, Serial2);
  });

  if (!usbSerial.setRxBufferSize(USB_SERIAL_RX_BUFFER_SIZE)) {
    Serial2.println("USB Serial RX buffer allocation failed");
  }
  if (!usbSerial.begin(USB_SERIAL_BAUDRATE)) {
    Serial2.println("USB Serial initialization failed");
  }
  if (!usb.begin()) {
    Serial2.printf("USB Host initialization failed: %s\n", usb.lastErrorName());
  } else {
    Serial2.println("USB Host initialized");
  }

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
  updateUsbConnectionState();
  handleUsbSerialInput();

  if (M5.BtnA.isPressed()) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.setTextColor(WHITE);
    M5.Display.println("画面クリア");
    M5.Display.println("EnOcean待機中...");
  }

  delay(1);
}
