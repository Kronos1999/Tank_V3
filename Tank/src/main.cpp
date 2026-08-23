// PS3 Controlled Tank with Servo and ESCs. with 3 rates of fire. single shot, burst, and auto. Usign FastLED to simulate a muzzle flash. 
#include <Arduino.h>
#include <Ps3Controller.h>
#include <ESP32Servo.h>
#include <FastLED.h>

#define NUM_LEDS 6
#define DATA_PIN 2 // Muzzle LED's

CRGB leds[NUM_LEDS];

// --- PIN ASSIGNMENTS ---
const int motorL_PWM = 25;
const int motorR_PWM = 27;
const int motorL_IN1 = 26;
const int motorL_IN2 = 14;
const int motorR_IN3 = 33;
const int motorR_IN4 = 32;

const int servoPin = 18;
const int escPin1 = 19;
const int escPin2 = 21;
const int elevPin = 22;
const int LED_PIN = 4;

const uint32_t SERIAL_LOG_INTERVAL_MS = 250;
const uint32_t AUTO_FIRE_INTERVAL_MS = 15; // Extra pause after each complete shot cycle
const uint32_t ESC_SPINUP_DELAY_MS = 1500; // Let flywheels reach speed before loading
const uint32_t SHOOT_PULSE_MS = 200; // Time held at 145 degrees
const uint32_t SERVO_RETURN_SETTLE_MS = 200; // Time held at 90 degrees
const uint32_t TRACTION_LOG_INTERVAL_MS = 1000;

enum FireMode {
    FIRE_MODE_NONE,
    FIRE_MODE_SINGLE,
    FIRE_MODE_BURST,
    FIRE_MODE_AUTO
};

bool lastSquareState = false;
bool ledStateOn = false;
FireMode activeFireMode = FIRE_MODE_NONE;
bool outputsInitialized = false;
uint8_t burstShotsFired = 0;
uint32_t nextShotAtMs = 0;
uint32_t lastSerialLogMs = 0;
uint32_t lastControllerStatusLogMs = 0;
uint8_t lastBatteryState = 255;

Servo myServo, myESC1, myESC2, elevServo, ledDriverBoard;
float elevPos = 90.0f;
const float elevMin = 45.0f;
const float elevMax = 135.0f;
const float moveSpeed = 0.6f;

void setLedStrip(CRGB color) {// muzzle flash
    fill_solid(leds, NUM_LEDS, color);
    FastLED.show();
}

void driveMotor(const char* side, int pwmPin, int in1, int in2, int speed) {
    (void)side;
    const int deadzone = 20;
    const int finalPWM = map(abs(speed), 0, 128, 0, 255);

    if (abs(speed) < deadzone) {
        analogWrite(pwmPin, 0);
        digitalWrite(in1, LOW);
        digitalWrite(in2, LOW);
        return;
    }

    analogWrite(pwmPin, finalPWM);

    if (speed > 0) {
        digitalWrite(in1, HIGH);
        digitalWrite(in2, LOW);
    } else {
        digitalWrite(in1, LOW);
        digitalWrite(in2, HIGH);
    }
}

void stopDriveMotors() {
    analogWrite(motorL_PWM, 0);
    analogWrite(motorR_PWM, 0);
    digitalWrite(motorL_IN1, LOW);
    digitalWrite(motorL_IN2, LOW);
    digitalWrite(motorR_IN3, LOW);
    digitalWrite(motorR_IN4, LOW);
}

void stopAllSystems() {
    stopDriveMotors();
    if (outputsInitialized) {
        myServo.write(90); // return load arm to ready
        elevServo.write(100); // barrell to level
        myESC1.write(0); // ESC's to stopped
        myESC2.write(0); // ESC's to stopped
        setLedStrip(CRGB::Black);
    }
    activeFireMode = FIRE_MODE_NONE;
    burstShotsFired = 0;
}

void rampEscsDown(int startValue, int stepSize, int stepDelayMs) {
    for (int s = startValue; s >= 0; s -= stepSize) {
        myESC1.write(s);
        myESC2.write(s);
        delay(stepDelayMs);
    }
}

void armEscs() {
    myESC1.write(140);
    myESC2.write(140);
    delay(100);
}

void updateBatteryStatus() {
    if (!Ps3.isConnected()) {
        return;
    }

    const uint8_t battery = Ps3.data.status.battery;
    if (battery == lastBatteryState) {
        return;
    }

    lastBatteryState = battery;
    Ps3.setPlayer(1);

    Serial.print("Controller battery: ");
    switch (battery) {
        case ps3_status_battery_charging:
            Serial.println("charging");
            break;
        case ps3_status_battery_full:
            Serial.println("FULL");
            break;
        case ps3_status_battery_high:
            Serial.println("HIGH");
            break;
        case ps3_status_battery_low:
            Serial.println("LOW");
            break;
        case ps3_status_battery_dying:
            Serial.println("DYING");
            Ps3.setRumble(100.0, 1000);
            break;
        default:
            Serial.println("unknown");
            break;
    }

    if (battery != ps3_status_battery_dying) {
        Ps3.setRumble(0.0, 0);
    }
}

void handleSquareButton() {// this doesn't work the LED's stay on all the time need to remove this and its dependancies
    const bool currentSquareState = Ps3.data.button.square;// this doesn't work the LED's stay on all the time need to remove this and its dependancies
    if (currentSquareState && !lastSquareState) {// this doesn't work the LED's stay on all the time need to remove this and its dependancies
        ledStateOn = !ledStateOn;// this doesn't work the LED's stay on all the time need to remove this and its dependancies
        ledDriverBoard.writeMicroseconds(ledStateOn ? 2000 : 1000); // this doesn't work the LED's stay on all the time need to remove this and its dependancies
    }
    lastSquareState = currentSquareState; // this doesn't work the LED's stay on all the time need to remove this and its dependancies
}

void fireShot(CRGB color) {
    myServo.write(145); // extend load arm
    setLedStrip(color);
    delay(SHOOT_PULSE_MS);

    myServo.write(90); // return load arm to ready
    setLedStrip(CRGB::Black);
    delay(SERVO_RETURN_SETTLE_MS); // not sure what this is..
}

void cancelActiveFireMode() {
    activeFireMode = FIRE_MODE_NONE;
    burstShotsFired = 0;
    myServo.write(90); //return load arm to ready
    setLedStrip(CRGB::Black);
}

void startFireMode(FireMode mode) {
    if (!Ps3.isConnected()) {
        return;
    }

    activeFireMode = mode;
    burstShotsFired = 0;
    stopDriveMotors();
    armEscs();
    nextShotAtMs = millis() + ESC_SPINUP_DELAY_MS;

    switch (mode) {
        case FIRE_MODE_SINGLE:
            Serial.println("EVENT: Cross fired (single shot)");
            break;
        case FIRE_MODE_BURST:
            Serial.println("EVENT: Circle fired (three shots)");
            break;
        case FIRE_MODE_AUTO:
            Serial.println("EVENT: Triangle full auto armed");
            break;
        default:
            break;
    }
}

void updateFireMode() {
    if (!Ps3.isConnected() || activeFireMode == FIRE_MODE_NONE) {
        return;
    }

    const uint32_t now = millis();

    if (activeFireMode == FIRE_MODE_SINGLE) {
        if (now >= nextShotAtMs) {
            fireShot(CRGB::Yellow);
            rampEscsDown(120, 5, 30); //slowing the ESC's down preventing induction loads. 
            activeFireMode = FIRE_MODE_NONE;
        }
        return;
    }

    if (activeFireMode == FIRE_MODE_BURST) {
        if (now >= nextShotAtMs) {
            fireShot(CRGB::Red);
            burstShotsFired++;
            nextShotAtMs = millis() + 160;// what does this do? is it a pause between shots? 

            if (burstShotsFired >= 3) {
                rampEscsDown(120, 5, 30);//slowing the ESC's down preventing induction loads
                activeFireMode = FIRE_MODE_NONE;
            }
        }
        return;
    }

    if (activeFireMode == FIRE_MODE_AUTO) {
        if (now >= nextShotAtMs) {
            fireShot(CRGB::Orange);
            nextShotAtMs = millis() + AUTO_FIRE_INTERVAL_MS;
        }
    }
}

void notify() {
    if (Ps3.event.button_down.cross) {
        startFireMode(FIRE_MODE_SINGLE);
    }

    if (Ps3.event.button_down.circle) {
        startFireMode(FIRE_MODE_BURST);
    }

    if (Ps3.event.button_down.triangle) {
        startFireMode(FIRE_MODE_AUTO);
    }

    if (Ps3.event.button_up.triangle) {
        Serial.println("EVENT: Triangle released, ramping down");
        cancelActiveFireMode();
        rampEscsDown(120, 5, 30); // slowing the ESC's down preventing a large induction load
    }

    updateBatteryStatus();
}

void onConnect() {
    Serial.println("SYSTEM: PS3 Controller Connected!");
    Ps3.setPlayer(1);
    updateBatteryStatus();
}

void onDisconnect() {
    Serial.println("SAFETY: Controller disconnected; all outputs stopped");
    stopAllSystems();
}

void setup() {
    Serial.begin(115200);
//these pins go to the LM298N motor driver board.
    pinMode(motorL_PWM, OUTPUT);
    pinMode(motorL_IN1, OUTPUT);
    pinMode(motorL_IN2, OUTPUT);
    pinMode(motorR_PWM, OUTPUT);
    pinMode(motorR_IN3, OUTPUT);
    pinMode(motorR_IN4, OUTPUT);

    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);// muzzle flash
    FastLED.setBrightness(255);
    setLedStrip(CRGB::Black);

    digitalWrite(motorL_IN1, LOW);
    digitalWrite(motorL_IN2, LOW);
    digitalWrite(motorR_IN3, LOW);
    digitalWrite(motorR_IN4, LOW);
    analogWrite(motorL_PWM, 0);
    analogWrite(motorR_PWM, 0);

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
// The ESC are driving drone motors. 
    myServo.attach(servoPin, 500, 2400);
    myESC1.attach(escPin1, 1000, 2000);
    myESC2.attach(escPin2, 1000, 2000);
    elevServo.attach(elevPin, 500, 2400);
// The LED driver board is for headlights. They stay on all the time. 
    ledDriverBoard.attach(LED_PIN, 1000, 2000);
    ledDriverBoard.writeMicroseconds(1000);

    myESC1.write(0);
    myESC2.write(0);
    delay(2000);

    myESC1.write(180);
    myESC2.write(180);
    delay(100);

    myESC1.write(0);
    myESC2.write(0);
    delay(1000);

    Serial.println("SYSTEM: ESCs armed");
    myServo.write(90);
    elevServo.write(90);
    setLedStrip(CRGB::Black);
    outputsInitialized = true;

    Ps3.attach(notify);
    Ps3.attachOnConnect(onConnect);
    Ps3.attachOnDisconnect(onDisconnect);
    Ps3.begin("38:4f:f0:00:e8:0a");
}

void loop() {
    if (!Ps3.isConnected()) {
        static unsigned long lastMsg = 0;
        if (millis() - lastMsg > 2000) {
            Serial.println("SYSTEM: Searching for Controller...");
            lastMsg = millis();
        }
        stopAllSystems();
        return;
    }

    handleSquareButton();

    const int lx = Ps3.data.analog.stick.lx;
    const int ly = -Ps3.data.analog.stick.ly;
    const int leftSpeed = constrain(ly + lx, -128, 128);
    const int rightSpeed = constrain(ly - lx, -128, 128);

    if (millis() - lastSerialLogMs > SERIAL_LOG_INTERVAL_MS) {
        Serial.printf("DEBUG: lx=%d, ly=%d, left=%d, right=%d\n", lx, ly, leftSpeed, rightSpeed);
        lastSerialLogMs = millis();
    }

    driveMotor("LEFT", motorL_PWM, motorL_IN1, motorL_IN2, leftSpeed);
    driveMotor("RIGHT", motorR_PWM, motorR_IN3, motorR_IN4, rightSpeed);

    const int ry = Ps3.data.analog.stick.ry;
    if (abs(ry) > 10) {
        elevPos -= (ry / 128.0f) * moveSpeed;
        elevPos = constrain(elevPos, elevMin, elevMax);
        elevServo.write((int)elevPos);
    }

    updateFireMode();
    updateBatteryStatus();

    if (millis() - lastControllerStatusLogMs > TRACTION_LOG_INTERVAL_MS) {
        Serial.println("SYSTEM: Controller connected and tracking battery/player state.");
        lastControllerStatusLogMs = millis();
    }

    delay(10);
}
