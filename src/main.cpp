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
#define i2c_Address 0x3c
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1



// Serial logging

//#define DEBUG     // Comment this line to disable serial logging

// This code was ripped straight from reddit
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

// Ammo counter display

// Pressure display


// Pressure transducer
const int pressure_transducer_pin = A0;



// ========== Display Functions ========================================================================================
/**
 * Template display animation that I liked so it's the boot animation
 */
void testfillrect(void) {
    uint8_t color = 1;
    for (int16_t i = 0; i < display.height() / 2; i += 3) {
        // alternate colors
        display.fillRect(i, i, display.width() - i * 2, display.height() - i * 2, color % 2);
        display.display();
        delay(1);
        color++;
    }
}
/**
 * Animation the displays show while booting
 */
void boot_animation(void) {
    testfillrect();
}



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

// The current pressure in the air tank in PSI
byte pressure = 0;
// The pressure that the compressor will bring the air tank to in PSI
volatile byte target_pressure = 0;
// The maximum pressure while the limiter is enabled in PSI
const byte max_limited_pressure = 50;
// The maximum pressure while the limiter is disabled in PSI
const byte max_unlimited_pressure = 100;



// Ammo counter

// The currently selected magazine size
byte max_ammo = 10;
// The remaining amount of darts
byte remaining_ammo = 10;
// Used to monitor the rotary encoder used to select the magazine size
byte ammo_encoder_last_state = 0;
byte ammo_encoder_current_state = 0;
// Used to update the display in the main loop if the ammo encoder was changed, since ISRs do not like serial prints.
byte ammo_encoder_updated = 0;



// Magazine

// Whether or not a magazine is inserted (0 if there is no magazine, 1 if there is one)
byte magazine_button_last_state = 0;
byte magazine_button_current_state = 0;



// ========== Ammo Counter Functions ===================================================================================
/**
 * Updates the ammo counter display
 */
void update_ammo_display() {

    char remaining_ammo_str[3];
    char max_ammo_str[3];

    itoa(remaining_ammo, remaining_ammo_str, 10);
    if (remaining_ammo < 10) {
        remaining_ammo_str[1] = remaining_ammo_str[0];
        remaining_ammo_str[0] = '0';
    }
    remaining_ammo_str[2] = NULL;

    itoa(max_ammo, max_ammo_str, 10);
    if (max_ammo < 10) {
        max_ammo_str[1] = max_ammo_str[0];
        max_ammo_str[0] = '0';
    }
    max_ammo_str[2] = NULL;


    display.setTextSize(4);
    display.setTextColor(SH110X_WHITE, SH110X_BLACK); // 'inverted' text

    display.setCursor(0, 20);
    display.print(remaining_ammo_str);

    display.setCursor(52, 20);
    display.print("/");

    display.setCursor(80, 20);
    display.print(max_ammo_str);
    display.display();
    display.clearDisplay();


    // TODO: Figure out what this is
    // display.setTextSize(4);
    // display.setTextColor(SH110X_WHITE, SH110X_BLACK); // 'inverted' text
    // display.setCursor(0, 20);
    // display.print(remaining_ammo_str);
    // display.display();
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

    ammo_encoder_updated = 1;
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

    ammo_encoder_updated = 1;
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


// ========== Setup ====================================================================================================
/**
 * Initialize everything necessary when the Arduino boots up.
 */
void setup() {
    // Set up serial monitoring
    Serial.begin(9600);

    // Configure displays

    // Wait for display to power on
    delay(250);
    display.begin(i2c_Address, true);

    // Display a cool animation while the gun initializes
    boot_animation();
    display.display();
    //delay(2000);
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


    ammo_encoder_last_state = digitalRead(ammo_encoder_clk_pin);

    // Configure ammo encoder interrupts
    attachInterrupt(digitalPinToInterrupt(ammo_encoder_clk_pin), update_ammo_encoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ammo_encoder_dt_pin), update_ammo_encoder, CHANGE);


    // Set defaults
    //reset_remaining_ammo();
    //enable_limiter();
    update_ammo_display();
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
 * Turn on the limiter and reduce the target pressure to the limit if it is higher.
 */
void enable_limiter() {
    DEBUG_PRINTLN("Limiter enabled");
    // if (pressure >) {

    // }
}

/**
 * Turn off the limiter and raise the target pressure to the currently selected pressure if it is higher
 */
void disable_limiter() {
    DEBUG_PRINTLN("Limiter disabled");

    // if (pressure >) {

    // }
}



// ========== Main Loop ================================================================================================
/**
 * Main loop of the program.
 */
void loop() {

    // Set the states of all the buttons
    trigger_state = digitalRead(trigger_switch_pin);
    cancel_state = digitalRead(cancel_button_pin);

    limiter_switch_current_state = digitalRead(limiter_switch_pin);

    magazine_button_current_state = digitalRead(magazine_button_pin);

    // Get the current pressure in the air tank
    pressure = (byte)(150 * (analogRead(pressure_transducer_pin) / 1023.0));
    //DEBUG_PRINT("Current pressure: ");
    //DEBUG_PRINT(pressure);
    //DEBUG_PRINTLN(" PSI");

    // Ammo can be changed in an ISR, so the display should be updated constantly.
    update_ammo_display();



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