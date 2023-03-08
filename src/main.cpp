//
// Created by ROBERT-PC on 2022-12-28.
//

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <stdlib.h>
//#include "../.pio/libdeps/uno/Adafruit SH110X/Adafruit_SH110X.h"



// ========== Setup ====================================================================================================

// Definitions

// OLED display
#define oled_display_i2c_address 0x3c
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1



// i2c
#define TCA95481_address 0x70      // The address of the i2c multiplexer



// The max pressure of the transducer. Depends on the model.
#define TRANSDUCER_MAX_PRESSURE_PSI 150


// Serial logging

// This code was ripped straight from reddit
//#define DEBUG     // Comment this line to disable serial logging
#ifdef DEBUG
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x) do {} while (0)
#define DEBUG_PRINTLN(x) do {} while (0)
#endif



/*
  idle state is when the gun is waiting for something to happen
  charging state is when the compressor is running and increasing the pressure
  charged state is when the max pressure has been reached
  canceled state is when a charge has been canceled but the trigger has not been released yet
*/
enum firing_state { idle, charging, charged, canceled };
enum firing_state fire_state = idle;
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);



// ========== Pin setup ================================================================================================
// Firing
const int trigger_switch_pin = 4;
const int cancel_button_pin = 10;
const int limiter_switch_pin = 8;

// Ammo counter
const int ammo_encoder_clk_pin = 2;
const int ammo_encoder_dt_pin = 3;
const int magazine_button_pin = 7;

// i2c
// pins
const int i2c_SDA_pin = A4;
const int i2c_SCL_pin = A5;
// Displays
const int ammo_display_i2c_multiplexer_bus = 7;
const int pressure_display_i2c_multiplexer_bus = 5;


// Pressure selector
const int pressure_select_pot_pin = A1;

// Pressure display


// Pressure transducer
const int pressure_transducer_pin = A0;



// ========== State setup ==============================================================================================
// Firing

// Whether or not the trigger is depressed
byte trigger_state = LOW;
// Whether or not the cancel button is depressed
byte cancel_state = LOW;



// Pressure

// Whether or not the limiter switch is flipped on
byte limiter_switch_last_state = LOW;
byte limiter_switch_current_state = LOW;
// Whether or not the limiter is enabled.
byte limiter_on = 1;

// The current pressure in the air tank in PSI
byte pressure = 0;
// The pressure that the compressor will bring the air tank to in PSI
volatile byte target_pressure = 0;
// The maximum pressure while the limiter is enabled in PSI
const byte max_limited_pressure = 50;
// The maximum pressure while the limiter is disabled in PSI
const byte max_unlimited_pressure = 100;

// The lowest measured voltage when there is no pressure in the tank. The transducer is not perfect so this is calibration.
const float transducer_offset = 0.4834;



// Ammo counter

// The currently selected magazine size
byte max_ammo = 10;
// The remaining amount of darts
byte remaining_ammo = 10;
// Used to monitor the rotary encoder used to select the magazine size
byte ammo_encoder_last_state = 0;
byte ammo_encoder_current_state = 0;



// Magazine

// Whether or not a magazine is inserted (0 if there is no magazine, 1 if there is one)
byte magazine_button_last_state = 0;
byte magazine_button_current_state = 0;



// ========== i2c Multiplexer Functions ================================================================================
/**
 * Set the i2c multiplexer to the specified bus
 * @param i The index of the bus to set the multiplexer to.
 */
void tcaselect(uint8_t i) {
    // Buses are from 0-7. Higher than that is an invalid input.
    if (i > 7) {
        return;
    }

    Wire.beginTransmission(TCA95481_address);
    Wire.write(1 << i);
    Wire.endTransmission();
}



// ========== General Display Functions ================================================================================
/**
 * Template display animation that I liked so it's the boot animation
 */
void testfillrect(int i) {
    uint8_t color = (i / 3) + 1;

    // alternate colors
    display.fillRect(i, i, display.width() - i * 2, display.height() - i * 2, color % 2);
    display.display();
}

/**
 * Animation the displays show while booting up
 */
void boot_animation(void) {
    // Alternate displays for every frame of the animation. This way it plays on both displays at the same time.
    for (int16_t i = 0; i < display.height() / 2; i += 3) {
        tcaselect(ammo_display_i2c_multiplexer_bus);
        testfillrect(i);

        tcaselect(pressure_display_i2c_multiplexer_bus);
        testfillrect(i);

        delay(1);
    }

}



// ========== Ammo Counter Functions ===================================================================================
/**
 * Updates the ammo counter display
 */
void update_ammo_display() {
    // Select the ammo display on the i2c multiplexer
    tcaselect(ammo_display_i2c_multiplexer_bus);

    char remaining_ammo_str[3];
    char max_ammo_str[3];

    // Convert the remaining ammo to a string, with preceding 0s for 1-digit numbers.
//    itoa(remaining_ammo, remaining_ammo_str, 10);
//    if (remaining_ammo < 10) {
//        remaining_ammo_str[1] = remaining_ammo_str[0];
//        remaining_ammo_str[0] = '0';
//    }
    sprintf(remaining_ammo_str, "%2d", remaining_ammo);

    remaining_ammo_str[2] = '\0';

    // Perform the same conversion on the max ammo.
//    itoa(max_ammo, max_ammo_str, 10);
//    if (max_ammo < 10) {
//        max_ammo_str[1] = max_ammo_str[0];
//        max_ammo_str[0] = '0';
//    }
    sprintf(max_ammo_str, "%2d", max_ammo);

    max_ammo_str[2] = '\0';

    // Configure text format
    display.setTextSize(4);
    display.setTextColor(SH110X_WHITE, SH110X_BLACK); // 'inverted' text

    // Display remaining ammo
    display.setCursor(0, 20);
    display.print(remaining_ammo_str);

    // Display divider
    display.setCursor(52, 20);
    display.print("/");

    // Display max ammo
    display.setCursor(80, 20);
    display.print(max_ammo_str);

    // Output to the display
    display.display();
    display.clearDisplay();
}

/**
 * Updates the ammo counter display and prints the current ammo count to the serial log
 */
void print_ammo() {
    DEBUG_PRINT("Remaining ammo: ");
    DEBUG_PRINT(remaining_ammo);
    DEBUG_PRINT("/");
    DEBUG_PRINTLN(max_ammo);
    update_ammo_display();
}

/**
 * Reduces the remaining ammo by 1 unless it is 0, in which case does nothing.
 * Used when firing the gun.
 */
void reduce_current_ammo() {
    if (remaining_ammo > 0) {
        remaining_ammo--;
    }
    print_ammo();
}

/**
 * Resets the remaining ammo to the max.
 * Used when reloading the gun.
 */
void reset_remaining_ammo() {
    remaining_ammo = max_ammo;
    print_ammo();
}

/**
 * Reduce the max ammo shown on the ammo counter by 1.
 * Used with the ammo encoder to select the magazine size.
 */
void decrease_max_ammo() {
    // Prevent the max ammo from going below zero
    if (max_ammo > 0) {
        max_ammo--;
    }

    // If the magazine is full, reduce the current remaining ammo as well.
    if (remaining_ammo > max_ammo) {
        remaining_ammo--;
    }
}

/**
 * Increase the max ammo shown on the ammo counter by 1.
 * Used with the ammo encoder to select the magazine size.
 */void increase_max_ammo() {
    // Limit ammo to 2 digit numbers (largest nerf magazine is 35 rounds anyways)
    if (max_ammo < 99) {
        max_ammo++;
    }

    // If the magazine is full, increase the current remaining ammo as well.
    if (remaining_ammo == max_ammo - 1) {
        remaining_ammo++;
    }
 }

/**
 * ISR to update the status of the ammo encoder.
 */
void update_ammo_encoder() {

    // Read the current state of the encoder's CLK pin
    ammo_encoder_current_state = digitalRead(ammo_encoder_clk_pin);

    if (ammo_encoder_current_state != ammo_encoder_last_state) {  // && ammo_encoder_current_state == 1) {
        if (digitalRead(ammo_encoder_dt_pin) != ammo_encoder_current_state) {
            increase_max_ammo();
        }
        else {
            decrease_max_ammo();
        }
    }
    ammo_encoder_last_state = ammo_encoder_current_state;
}



// ========== Pressure display functions ===============================================================================
/**
 * Displays a series of vertical bars to indicate the current pressure.
 * There are 8 bars, each representing 1/8 of the target pressure. They fill from bottom to top as the pressure rises.
 * The last bar fills a little bit early to display a bit more consistently.
 * The higher bars are wider.
 */
void display_pressure_bar() {
    // Display current pressure as a progress bar from 0 to the target pressure

    // 7.5/8 of the way to the target
    if (pressure >= target_pressure * 7.5 / 8.0) {
        display.fillRect(0, 0, 40, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 0, 40, 6, SH110X_WHITE);
    }

    // 7/8 of the way to the target
    if (pressure >= target_pressure * 7 / 8.0) {
        display.fillRect(0, 8, 30, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 8, 30, 6, SH110X_WHITE);

    }

    // 6/8 of the way to the target
    if (pressure >= target_pressure * 6 / 8.0) {
        display.fillRect(0, 16, 25, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 16, 25, 6, SH110X_WHITE);
    }

    // 5/8 of the way to the target
    if (pressure >= target_pressure * 5 / 8.0) {
        display.fillRect(0, 24, 22, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 24, 22, 6, SH110X_WHITE);
    }

    // 4/8 of the way to the target
    if (pressure >= target_pressure * 4 / 8.0) {
        display.fillRect(0, 32, 19, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 32, 19, 6, SH110X_WHITE);
    }

    // 3/8 of the way to the target
    if (pressure >= target_pressure * 3 / 8.0) {
        display.fillRect(0, 40, 18, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 40, 18, 6, SH110X_WHITE);
    }

    // 2/8 of the way to the target
    if (pressure >= target_pressure * 2 / 8.0) {
        display.fillRect(0, 48, 17, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 48, 17, 6, SH110X_WHITE);
    }

    // 1/8 of the way to the target
    if (pressure >=  target_pressure / 8.0) {
        display.fillRect(0, 56, 17, 6, SH110X_WHITE);
    }
    else {
        display.drawRect(0, 56, 17, 6, SH110X_WHITE);
    }
}

/**
 * Updates the pressure display
 */
void update_pressure_display() {
    // Select the ammo display on the i2c multiplexer
    tcaselect(pressure_display_i2c_multiplexer_bus);

    char target_pressure_str[4];
    char current_pressure_str[4];

    // Convert the remaining ammo to a string.
    sprintf(target_pressure_str, "%03d", target_pressure);
    target_pressure_str[3] = '\0';

    // Perform the same conversion on the max ammo.
    sprintf(current_pressure_str, "%03d", pressure);
    current_pressure_str[3] = '\0';

    // Configure text format
    display.setTextSize(4);
    display.setTextColor(SH110X_WHITE, SH110X_BLACK); // 'inverted' text

    // Display target pressure numerically
    display.setCursor(52, 20);
    display.print(target_pressure_str);

    // Display the current pressure as a progress bar towards the target pressure.
    display_pressure_bar();

    // Output to the display
    display.display();
    display.clearDisplay();
}


// ========== Setup ====================================================================================================
/**
 * Initialize everything necessary when the Arduino boots up.
 */
void setup() {
    // Set up serial monitoring
    Serial.begin(9600);



    // Configure displays

    // Wait for displays to power on
    delay(250);

    // Initialize ammo display
    tcaselect(ammo_display_i2c_multiplexer_bus);
    display.begin(oled_display_i2c_address, true);

    // Initialize pressure display
    tcaselect(pressure_display_i2c_multiplexer_bus);
    display.begin(oled_display_i2c_address, true);

    // Display a cool animation while the gun initializes
    boot_animation();
    display.display();
    display.clearDisplay();



    // Configure pins

    // Firing
    pinMode(trigger_switch_pin, INPUT_PULLUP);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(cancel_button_pin, INPUT_PULLUP);

    // Pressure
    pinMode(limiter_switch_pin, INPUT_PULLUP);

    // Ammo counter
    pinMode(ammo_encoder_clk_pin, INPUT);
    pinMode(ammo_encoder_dt_pin, INPUT);
    pinMode(magazine_button_pin, INPUT_PULLUP);



    // Initialize values
    ammo_encoder_last_state = digitalRead(ammo_encoder_clk_pin);
    target_pressure = analogRead(pressure_select_pot_pin);
//    reset_remaining_ammo();
    limiter_switch_last_state = 0;
    update_ammo_display();
    update_pressure_display();



    // Configure ammo encoder interrupts
    attachInterrupt(digitalPinToInterrupt(ammo_encoder_clk_pin), update_ammo_encoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ammo_encoder_dt_pin), update_ammo_encoder, CHANGE);
}



// ========== Gun Functions ============================================================================================
/**
 * Fire the gun by opening the pilot solenoid valve
 */
void fire() {
    DEBUG_PRINTLN("Firing");

    // Fire gun
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);

    // Update ammo counter
    reduce_current_ammo();
}

/**
 * Cancel a shot by releasing air from the air tank to atmosphere by opening the cancel valve.
 */
void cancel() {
    DEBUG_PRINTLN("Canceling");
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);
}



// ========== Pressure Limiter Functions ===============================================================================
/**
 * Turn on the limiter.
 */
void enable_limiter() {
    DEBUG_PRINTLN("Limiter enabled");
    limiter_on = 1;
}

/**
 * Turn off the limiter.
 */
void disable_limiter() {
    DEBUG_PRINTLN("Limiter disabled");
    limiter_on = 0;
}



// ========== Pressure Selector Functions ==============================================================================
void update_target_pressure(int signal) {
    // Analog signals are a range from 0-1023, representing 0-5 volts.
    double voltage =  signal * 5.00 / 1024;

    // The potentiometer outputs 0 V at 0% rotated and 5 V at 100% rotated.
    target_pressure = (byte)(voltage * (max_unlimited_pressure / 5.00));

    // Apply limiter if it is on
    if (limiter_on && target_pressure > max_limited_pressure) {
        target_pressure = max_limited_pressure;
    }

    // Apply hard limiter. This code shouldn't be reachable, but if something goes wrong this will prevent overfilling the tank.
    if (target_pressure > max_unlimited_pressure) {
        target_pressure = max_unlimited_pressure;
    }
}


// ========== Pressure Transducer Functions ============================================================================

byte voltage_to_pressure_psi(double voltage) {
    // The pressure transducer outputs 0.5 V at 0 PSI and 4.5 V at 150 PSI.
    // Subtracting the offset gives us a range of 0-4 V == 0-150 PSI.
    return (byte)((voltage - transducer_offset) * (TRANSDUCER_MAX_PRESSURE_PSI / 4.00));
}

void update_pressure(int signal) {
    // Analog signals are a range from 0-1023, representing 0-5 volts.
    double voltage =  signal * 5.00 / 1024;
    pressure = voltage_to_pressure_psi(voltage);
    if (pressure < 0) {
        // If the transducer is calibrated correctly this should never happen, but just in case.
        pressure = 0;
    }
}


// ========== Main Loop ================================================================================================
/**
 * Main loop of the program.
 */
void loop() {
    // Set the states of all the components
    trigger_state = digitalRead(trigger_switch_pin);
    cancel_state = digitalRead(cancel_button_pin);
    limiter_switch_current_state = digitalRead(limiter_switch_pin);
    magazine_button_current_state = digitalRead(magazine_button_pin);
    // Read the pressure in the tank.
    update_pressure(analogRead(pressure_transducer_pin));
    // Read the pressure the pressure selector is set to
    update_target_pressure(analogRead(pressure_select_pot_pin));



    // Ammo can be changed in an ISR, so the display should be updated constantly.
    update_ammo_display();
    // Pressure changes frequently
    update_pressure_display();

    // ========== Trigger ==============================================================================================
    // Trigger is depressed
    if (trigger_state == HIGH) {
        // Trigger has just been pressed, begin charging gun
        if (fire_state == idle) {
            DEBUG_PRINTLN("Trigger pressed");
            DEBUG_PRINTLN("Starting charging");
            fire_state = charging;
        }
    }
        // Trigger is not depressed
    else {
        // Trigger has been released, fire gun
        if (fire_state == charging || fire_state == charged) {
            DEBUG_PRINTLN("Trigger released");
            fire();
            fire_state = idle;
        }
            // Trigger has been released after canceling shot
        else if (fire_state == canceled) {
            DEBUG_PRINTLN("Trigger released after canceling");
            // Ensures a smooth transition while the physical switch is moving
            delay(5);
            fire_state = idle;
        }
    }

    // ========== Cancel button ========================================================================================
    // Cancel button is depressed
    if (cancel_state == LOW) {
        // Gun is in a state that can be canceled from
        if (fire_state == charging || fire_state == charged) {
            DEBUG_PRINTLN("Cancel button pressed");
            cancel();
            fire_state = canceled;
        }
    }

    // ========== Magazine =============================================================================================
    // Change in magazine status
    if (magazine_button_last_state != magazine_button_current_state) {
        // Magazine has been inserted
        if (magazine_button_current_state == HIGH) {
            DEBUG_PRINTLN("Magazine inserted");
            reset_remaining_ammo();
        }
        else {
            DEBUG_PRINTLN("Magazine removed");
            remaining_ammo = 0;
            print_ammo();
            // Ensures a smooth transition while the physical switch is moving
            delay(5);
        }
        magazine_button_last_state = magazine_button_current_state;
    }

    // ========== Limiter ==============================================================================================
    // Change in limiter status
    if (limiter_switch_current_state != limiter_switch_last_state) {
        if (limiter_switch_current_state == HIGH) {
            enable_limiter();
            // Switch flickers a few times without this delay
            delay(5);
        }
        else {
            disable_limiter();
            // Switch flickers a few times without this delay
            delay(5);
        }
        limiter_switch_last_state = limiter_switch_current_state;
    }

}