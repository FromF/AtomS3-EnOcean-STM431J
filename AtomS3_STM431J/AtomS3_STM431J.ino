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
 *   - USB CDC-ACM経由でUSB400Jと通信
 *   - EnOcean ESP3プロトコル解析
 *   - RADIO_ERP2の32bit/48bit Originator ID対応
 *   - EEP A5-02-05（温度センサー）対応
 *   - デバイスIDと温度（℃）をディスプレイ表示
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
 *   - USB400J: 57600bps, 8N1, USB CDC-ACM
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
#define USB_SERIAL_RX_BUFFER_SIZE 1024
#define ESP3_RECEIVE_BUFFER_SIZE 1024
#define ESP3_HEADER_SIZE 6
#define ESP3_SYNC_BYTE 0x55
#define ESP3_PACKET_TYPE_RADIO_ERP2 0x0A
#define ERP2_ADDRESS_CONTROL_MASK 0xE0
#define ERP2_EXTENDED_HEADER_FLAG 0x10
#define ERP2_RORG_MASK 0x0F
#define ERP2_RORG_4BS 0x02
#define FOUR_BS_DATA_SIZE 4
#define FOUR_BS_LEARN_BIT 0x08

EspUsbHost usb;
EspUsbHostCdcSerial usbSerial(usb);
bool isConnected = false;

// USBの読み出し境界に依存せず、完全なESP3パケットになるまで保持する
uint8_t esp3ReceiveBuffer[ESP3_RECEIVE_BUFFER_SIZE];
size_t esp3ReceiveLength = 0;

// EnOceanパケット構造体
struct EnOceanPacket {
  uint16_t dataLength;
  uint8_t optionalLength;
  uint8_t packetType;
  uint8_t data[256];
  uint8_t optionalData[256];
  uint64_t senderId;
  uint8_t senderIdLength;
  float temperature;
  bool teachIn;
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

uint64_t readBigEndianId(const uint8_t *data, size_t length) {
  uint64_t value = 0;
  for (size_t i = 0; i < length; i++) {
    value = (value << 8) | data[i];
  }
  return value;
}

bool getErp2AddressLengths(uint8_t erp2Header, size_t &originatorLength,
                           size_t &destinationLength) {
  destinationLength = 0;
  switch (erp2Header & ERP2_ADDRESS_CONTROL_MASK) {
    case 0x00:
      originatorLength = 3;
      return true;
    case 0x20:
      originatorLength = 4;
      return true;
    case 0x40:
      originatorLength = 4;
      destinationLength = 4;
      return true;
    case 0x60:
      originatorLength = 6;
      return true;
    default:
      return false;
  }
}

void formatSenderId(uint64_t senderId, uint8_t senderIdLength,
                    char *output, size_t outputSize) {
  if (senderIdLength <= 4) {
    snprintf(output, outputSize, "%08lX", (unsigned long)senderId);
    return;
  }

  snprintf(output, outputSize, "%04lX%08lX",
           (unsigned long)((senderId >> 32) & 0xFFFF),
           (unsigned long)(senderId & 0xFFFFFFFFULL));
}

void printHexBytes(const char *label, const uint8_t *data, size_t length) {
  Serial2.printf("%s (%u bytes): ", label, (unsigned int)length);
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 16) Serial2.print('0');
    Serial2.print(data[i], HEX);
    Serial2.print(' ');
  }
  Serial2.println();
}

/**
 * @brief EnOceanパケット解析
 */
bool parseEnOceanPacket(const uint8_t *buffer, size_t len, EnOceanPacket &packet) {
  packet.valid = false;
  packet.teachIn = false;

  if (len < ESP3_HEADER_SIZE) return false;
  if (buffer[0] != ESP3_SYNC_BYTE) return false;  // 同期バイトチェック

  packet.dataLength = ((uint16_t)buffer[1] << 8) | buffer[2];
  packet.optionalLength = buffer[3];
  packet.packetType = buffer[4];

  // パケット全体の長さチェック
  size_t totalLen = ESP3_HEADER_SIZE + packet.dataLength + packet.optionalLength + 1;
  if (len < totalLen) return false;
  if (packet.dataLength > sizeof(packet.data)) {
    Serial2.println("ESP3 payload too large");
    return false;
  }

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

  if (packet.packetType != ESP3_PACKET_TYPE_RADIO_ERP2 || packet.dataLength < 2) {
    return false;
  }

  // RADIO_ERP2のDATA部にはLENGTHを除いたERP2 telegramが格納される
  const uint8_t erp2Header = packet.data[0];
  if ((erp2Header & ERP2_RORG_MASK) != ERP2_RORG_4BS) {
    return false;
  }

  size_t cursor = 1;
  uint8_t radioOptionalLength = 0;
  if ((erp2Header & ERP2_EXTENDED_HEADER_FLAG) != 0) {
    if (cursor >= packet.dataLength - 1) return false;
    radioOptionalLength = packet.data[cursor++] & 0x0F;
  }

  size_t originatorLength = 0;
  size_t destinationLength = 0;
  if (!getErp2AddressLengths(erp2Header, originatorLength, destinationLength)) {
    return false;
  }

  // DATA末尾の1バイトはERP2自身のCRC
  const size_t erp2CrcOffset = packet.dataLength - 1;
  if (radioOptionalLength > erp2CrcOffset - cursor) return false;
  const size_t radioDataEnd = erp2CrcOffset - radioOptionalLength;
  if (cursor + originatorLength + destinationLength > radioDataEnd) return false;

  packet.senderId = readBigEndianId(&packet.data[cursor], originatorLength);
  packet.senderIdLength = originatorLength;
  cursor += originatorLength + destinationLength;

  if (radioDataEnd - cursor != FOUR_BS_DATA_SIZE) {
    return false;
  }

  const uint8_t *fourBsData = &packet.data[cursor];
  packet.teachIn = (fourBsData[3] & FOUR_BS_LEARN_BIT) == 0;

  char senderIdText[13];
  formatSenderId(packet.senderId, packet.senderIdLength,
                 senderIdText, sizeof(senderIdText));
  if (packet.teachIn) {
    packet.valid = true;
    Serial2.printf("Teach-in telegram from Device ID: %s\n", senderIdText);
    return true;
  }

  // EEP A5-02-05: DB1の255..0を0..40℃へ変換
  const uint8_t temperatureRaw = fourBsData[2];
  packet.temperature = 40.0f - ((float)temperatureRaw * 40.0f / 255.0f);
  packet.valid = true;

  Serial2.printf("Device ID: %s, Temp: %.1f C\n",
                 senderIdText, packet.temperature);
  return true;
}

void displayEnOceanPacket(const EnOceanPacket &packet) {
  char senderIdText[13];
  formatSenderId(packet.senderId, packet.senderIdLength,
                 senderIdText, sizeof(senderIdText));

  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);

  M5.Display.println("EnOcean受信:");
  M5.Display.println();

  M5.Display.printf("ID: %s\n", senderIdText);
  M5.Display.println();

  if (packet.teachIn) {
    M5.Display.setTextSize(2);
    M5.Display.println("Teach-in");
    return;
  }

  M5.Display.setTextSize(2);
  M5.Display.printf("温度: %.1f C\n", packet.temperature);
}

void discardEsp3ReceiveBytes(size_t count) {
  if (count >= esp3ReceiveLength) {
    esp3ReceiveLength = 0;
    return;
  }

  memmove(esp3ReceiveBuffer, &esp3ReceiveBuffer[count], esp3ReceiveLength - count);
  esp3ReceiveLength -= count;
}

void clearEsp3ReceiveBuffer() {
  esp3ReceiveLength = 0;
}

/**
 * @brief 蓄積済みデータから完全なESP3パケットをすべて取り出す
 */
void processEsp3ReceiveBuffer() {
  while (esp3ReceiveLength > 0) {
    // 同期ずれしたデータを捨て、次の0x55をパケット先頭候補にする
    size_t syncOffset = 0;
    while (syncOffset < esp3ReceiveLength &&
           esp3ReceiveBuffer[syncOffset] != ESP3_SYNC_BYTE) {
      syncOffset++;
    }
    if (syncOffset > 0) {
      Serial2.printf("ESP3 resync: discarded %u byte(s)\n", (unsigned int)syncOffset);
      discardEsp3ReceiveBytes(syncOffset);
    }

    // ヘッダーが分割されている場合は次の受信を待つ
    if (esp3ReceiveLength < ESP3_HEADER_SIZE) {
      return;
    }

    if (calcCRC8(&esp3ReceiveBuffer[1], 4) != esp3ReceiveBuffer[5]) {
      Serial2.println("Header CRC error; resynchronizing");
      discardEsp3ReceiveBytes(1);
      continue;
    }

    const uint16_t dataLength =
      ((uint16_t)esp3ReceiveBuffer[1] << 8) | esp3ReceiveBuffer[2];
    const uint8_t optionalLength = esp3ReceiveBuffer[3];
    const size_t packetLength = ESP3_HEADER_SIZE +
                                (size_t)dataLength + optionalLength + 1;

    if (packetLength > ESP3_RECEIVE_BUFFER_SIZE) {
      Serial2.printf("ESP3 packet too large: %u byte(s)\n", (unsigned int)packetLength);
      discardEsp3ReceiveBytes(1);
      continue;
    }

    // データ部が分割されている場合は完全なパケットになるまで保持する
    if (esp3ReceiveLength < packetLength) {
      return;
    }

    // USBの読み出し境界ではなく、再構築済みのESP3パケットを1行で表示
    printHexBytes("ESP3 packet", esp3ReceiveBuffer, packetLength);

    const size_t payloadLength = (size_t)dataLength + optionalLength;
    const uint8_t expectedDataCrc =
      esp3ReceiveBuffer[ESP3_HEADER_SIZE + payloadLength];
    const uint8_t calculatedDataCrc =
      calcCRC8(&esp3ReceiveBuffer[ESP3_HEADER_SIZE], payloadLength);
    if (calculatedDataCrc == expectedDataCrc) {
      EnOceanPacket packet = {};
      if (parseEnOceanPacket(esp3ReceiveBuffer, packetLength, packet)) {
        displayEnOceanPacket(packet);
      }
    } else {
      Serial2.printf("Invalid ESP3 data CRC: expected 0x%02X, calculated 0x%02X\n",
                     expectedDataCrc, calculatedDataCrc);
    }

    // CRC不正時もヘッダーで算出したパケット長を捨て、次のパケットへ進む
    discardEsp3ReceiveBytes(packetLength);
  }
}

/**
 * @brief EspUsbHostの受信リングから読み出したデータを一時バッファへ蓄積
 */
void processReceivedData(const uint8_t *data, size_t dataLength) {
  // USB CDCから今回読み出せた単位。ESP3パケット境界とは一致しない場合がある
  printHexBytes("USB chunk", data, dataLength);

  size_t offset = 0;
  while (offset < dataLength) {
    processEsp3ReceiveBuffer();

    size_t available = ESP3_RECEIVE_BUFFER_SIZE - esp3ReceiveLength;
    if (available == 0) {
      Serial2.println("ESP3 receive buffer overflow; resynchronizing");
      discardEsp3ReceiveBytes(1);
      continue;
    }

    size_t copyLength = dataLength - offset;
    if (copyLength > available) {
      copyLength = available;
    }
    memcpy(&esp3ReceiveBuffer[esp3ReceiveLength], &data[offset], copyLength);
    esp3ReceiveLength += copyLength;
    offset += copyLength;
  }

  processEsp3ReceiveBuffer();
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
  if (!isConnected && esp3ReceiveLength > 0) {
    Serial2.printf("Discarding %u byte(s) of incomplete ESP3 data\n",
                   (unsigned int)esp3ReceiveLength);
    clearEsp3ReceiveBuffer();
  }
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
