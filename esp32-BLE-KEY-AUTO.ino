#include <BleKeyboard.h>
#include <Preferences.h>

// ============== 调试开关（发布固件时注释此行以关闭所有串口输出） ==============
#define DEBUG

#ifdef DEBUG
  #define LOG(x) Serial.println(x)
#else
  #define LOG(x)
#endif

// ============== 引脚定义（针对 ESP32-C3 进行了安全调整） ==============
#define PIN_BTN_PREV   8 // 上一曲按钮（避开 9 号 BOOT 脚）
#define PIN_BTN_NEXT   9  // 下一曲按钮（避开 9 号 BOOT 脚）
#define PIN_LED        1  // 状态指示灯（避开 0, 1 等特殊脚）

// ============== 时间常量（ms） ==============
#define DEBOUNCE_MS        50
#define LONG_PRESS_MS      2000
#define MEDIA_KEY_HOLD_MS  80    // 适当延长到 80ms，iOS 识别更稳定
#define SLOW_BLINK_MS      1000  // 已断开：慢闪
#define FAST_BLINK_MS      200   // 配对中：快闪
#define PAIRING_TIMEOUT_MS 30000 // 配对超时后转为等待重连

// ============== BLE 键盘初始化 ==============
// 只传 3 个标准参数，防止部分版本的库在构造时崩溃
BleKeyboard bleKeyboard("CarMediaKey", "Logitech", 100);
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
unsigned long lastBlinkTime    = 0;
unsigned long pairingStartTime = 0;
bool ledOn = false;

// ============== 媒体键发送状态（非阻塞） ==============
MediaKeyReport pendingKey; // 直接使用库自带的类型
unsigned long keyPressTime   = 0;
bool          keyPending     = false;

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
  delay(1000); // 给串口和硬件一点稳压准备时间
  LOG("ESP32 BLE Media Keyboard Starting...");

  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // 只读模式打开 Preferences
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

  // 必须先 begin，让蓝牙堆栈跑起来
  bleKeyboard.begin();
  
  // 随后注入 VID 和 PID 伪装
  bleKeyboard.set_vendor_id(0x046D);  // 罗技 VID
  bleKeyboard.set_product_id(0xB30B); // 罗技 PID
  
  LOG("BLE Keyboard started.");
}

void loop() {
  updateButton(btnPrev);
  updateButton(btnNext);

  // 媒体键非阻塞释放
  if (keyPending && millis() - keyPressTime >= MEDIA_KEY_HOLD_MS) {
    bleKeyboard.release(pendingKey);
    keyPending = false;
    LOG("<< Key Released");
  }

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
      LOG("Pairing timeout, switched to disconnected state.");
      setState(STATE_DISCONNECTED);
    }
  }

  if (btnPrev.pressed) {
    btnPrev.pressed = false;
    if (connected) {
      LOG(">> Previous Track");
      sendMediaKey(KEY_MEDIA_PREVIOUS_TRACK);
    } else {
      LOG("Warning: Not connected, key ignored.");
    }
  }

  if (btnNext.pressed) {
    btnNext.pressed = false;
    if (connected) {
      LOG(">> Next Track");
      sendMediaKey(KEY_MEDIA_NEXT_TRACK);
    } else {
      LOG("Warning: Not connected, key ignored.");
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

void setState(BleState newState) {
  currentState = newState;
  if (newState == STATE_PAIRING) {
    pairingStartTime = millis();
  }
}

// 发送媒体键
void sendMediaKey(const MediaKeyReport key) {
  if (keyPending) {
    bleKeyboard.release(pendingKey);
  }
  pendingKey[0] = key[0];
  pendingKey[1] = key[1];
  
  bleKeyboard.press(pendingKey);
  keyPressTime = millis();
  keyPending   = true;
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
  digitalWrite(PIN_LED, HIGH); // 亮灯提示正在重启
  delay(800);
  ESP.restart();
}