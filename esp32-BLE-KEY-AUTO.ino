#include <BleKeyboard.h>
#include <Preferences.h>

// ============== 调试开关（发布固件时注释此行以关闭所有串口输出） ==============
#define DEBUG

#ifdef DEBUG
  #define LOG(x) Serial.println(x)
#else
  #define LOG(x)
#endif

// ============== 引脚定义 ==============
#define PIN_BTN_PREV   8  // 上一曲按钮
#define PIN_BTN_NEXT   9   // 下一曲按钮（BOOT）
#define PIN_LED        0  // 状态指示灯

// ============== 时间常量（ms） ==============
#define DEBOUNCE_MS        50
#define LONG_PRESS_MS      2000
#define MEDIA_KEY_HOLD_MS  50    // iOS 需要至少 50ms 才能识别媒体键
#define SLOW_BLINK_MS      1000  // 已断开：慢闪
#define FAST_BLINK_MS      200   // 配对中：快闪
#define PAIRING_TIMEOUT_MS 30000 // 配对超时后转为等待重连

// ============== BLE 键盘 ==============
// VID/PID 伪装为罗技设备，避免 iOS 使用私有 Apple 协议解析导致媒体键失效
BleKeyboard bleKeyboard("ESP32 MediaKey", "ESP32", 100);
Preferences preferences;

// ============== 状态枚举 ==============
enum BleState : uint8_t {
  STATE_DISCONNECTED,
  STATE_PAIRING,
  STATE_CONNECTED
};
BleState currentState = STATE_DISCONNECTED;

// ============== 按钮结构体 ==============
struct Button {
  uint8_t pin;
  bool lastState;
  bool currentState;
  unsigned long lastDebounceTime;
  unsigned long pressStartTime;
  bool pressed;
  bool longPressed;
};

Button btnPrev  = { PIN_BTN_PREV, HIGH, HIGH, 0, 0, false, false };
Button btnNext  = { PIN_BTN_NEXT, HIGH, HIGH, 0, 0, false, false };

// ============== LED / 状态变量 ==============
unsigned long lastBlinkTime  = 0;
unsigned long pairingStartTime = 0;
bool ledOn = false;

// ============== 函数声明 ==============
void updateButton(Button &btn);
void handleLED();
void sendMediaKey(const MediaKeyReport key);
void setState(BleState newState);
void clearBondingData();

// --------------------------------------------------------------------------

void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif
  LOG("ESP32 BLE Media Keyboard Starting...");

  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // 只读模式打开 Preferences，读完立即关闭
  preferences.begin("ble_media", true);
  bool hasBonded = preferences.getBool("bonded", false);
  preferences.end();

  if (hasBonded) {
    LOG("Found bonded device, will auto-reconnect.");
    setState(STATE_DISCONNECTED);
  } else {
    LOG("No bonded device, entering pairing mode.");
    setState(STATE_PAIRING);
  }

  bleKeyboard.set_vendor_id(0x046D);  // 罗技 VID
  bleKeyboard.set_product_id(0xB30B);
  bleKeyboard.begin();
  LOG("BLE Keyboard started.");
}

void loop() {
  updateButton(btnPrev);
  updateButton(btnNext);

  bool connected = bleKeyboard.isConnected();

  if (connected && currentState != STATE_CONNECTED) {
    setState(STATE_CONNECTED);
    LOG("Device connected!");
    preferences.begin("ble_media", false);
    preferences.putBool("bonded", true);
    preferences.end();
  } else if (!connected && currentState == STATE_CONNECTED) {
    setState(STATE_DISCONNECTED);
    LOG("Device disconnected, waiting for reconnect...");
  } else if (!connected && currentState == STATE_PAIRING) {
    if (millis() - pairingStartTime > PAIRING_TIMEOUT_MS) {
      setState(STATE_DISCONNECTED);
    }
  }

  if (btnPrev.pressed) {
    btnPrev.pressed = false;
    if (connected) {
      LOG(">> Previous Track");
      sendMediaKey(KEY_MEDIA_PREVIOUS_TRACK);
    }
  }

  if (btnNext.pressed) {
    btnNext.pressed = false;
    if (connected) {
      LOG(">> Next Track");
      sendMediaKey(KEY_MEDIA_NEXT_TRACK);
    }
  }

  // 两键同时长按 → 清除配对数据并重启
  if (btnPrev.longPressed && btnNext.longPressed) {
    btnPrev.longPressed = false;
    btnNext.longPressed = false;
    LOG(">> Both buttons long-pressed: Reset Bluetooth bonding data!");
    clearBondingData();
  }

  handleLED();
}

// --------------------------------------------------------------------------

// 集中管理状态切换，自动维护 pairingStartTime
void setState(BleState newState) {
  currentState = newState;
  if (newState == STATE_PAIRING) {
    pairingStartTime = millis();
  }
}

// 发送媒体键：press + hold + release，确保 iOS 能识别
void sendMediaKey(const MediaKeyReport key) {
  bleKeyboard.press((uint8_t*)key);
  delay(MEDIA_KEY_HOLD_MS);
  bleKeyboard.release((uint8_t*)key);
}

void updateButton(Button &btn) {
  bool reading = digitalRead(btn.pin);
  if (reading != btn.lastState) {
    btn.lastDebounceTime = millis();
  }
  if ((millis() - btn.lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != btn.currentState) {
      btn.currentState = reading;
      if (btn.currentState == LOW) {
        btn.pressStartTime = millis();
        btn.longPressed = false;
      } else if (!btn.longPressed) {
        btn.pressed = true;
      }
    }
    if (btn.currentState == LOW && !btn.longPressed) {
      if (millis() - btn.pressStartTime >= LONG_PRESS_MS) {
        btn.longPressed = true;
      }
    }
  }
  btn.lastState = reading;
}

void handleLED() {
  unsigned long now = millis();
  switch (currentState) {
    case STATE_DISCONNECTED:
      if (now - lastBlinkTime >= SLOW_BLINK_MS) {
        lastBlinkTime = now;
        ledOn = !ledOn;
        digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
      }
      break;
    case STATE_PAIRING:
      if (now - lastBlinkTime >= FAST_BLINK_MS) {
        lastBlinkTime = now;
        ledOn = !ledOn;
        digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
      }
      break;
    case STATE_CONNECTED:
      // 仅在首次进入已连接状态时熄灭 LED，后续不重复写
      if (ledOn) {
        ledOn = false;
        digitalWrite(PIN_LED, LOW);
      }
      break;
  }
}

void clearBondingData() {
  preferences.begin("ble_media", false);
  preferences.clear();
  preferences.end();
  LOG("Bonding data cleared. Restarting...");
  delay(500);
  ESP.restart();
}