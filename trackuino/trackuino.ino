/* trackuino copyright (C) 2010  EA5HAV Javi
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

// Mpide 22 fails to compile Arduino code because it stupidly defines ARDUINO 
// as an empty macro (hence the +0 hack). UNO32 builds are fine. Just use the
// real Arduino IDE for Arduino builds. Optionally complain to the Mpide
// authors to fix the broken macro.
#if (ARDUINO + 0) == 0
#error "Oops! We need the real Arduino IDE (version 22 or 23) for Arduino builds."
#error "See trackuino.pde for details on this"

// Refuse to compile on arduino version 21 or lower. 22 includes an 
// optimization of the USART code that is critical for real-time operation
// of the AVR code.
#elif (ARDUINO + 0) < 22
#error "Oops! We need Arduino 22 or 23"
#error "See trackuino.pde for details on this"

#endif


// Trackuino custom libs
#include "config.h"
#include "afsk_avr.h"
#include "afsk_avr32u4.h"
#include "afsk_pic32.h"
#include "aprs.h"
#include "buzzer.h"
#include "gps.h"
#include "pin.h"
#include "power.h"
#include "sensors_avr.h"
#include "sensors_pic32.h"

// I stole these macros from one of the 32u4 bootloaders.  Needed because
// I wired the leoTracker USB Activity LED "backwards".  I hope this will
// be fixed with the new bootloader
#define TX_LED_OFF()	PORTD |= (1<<5)
#define TX_LED_ON()	    PORTD &= ~(1<<5)
#define RX_LED_OFF()	PORTB |= (1<<0)
#define RX_LED_ON()	    PORTB &= ~(1<<0)

// Arduino/AVR libs
#if (ARDUINO + 1) >= 100
#  include <Arduino.h>
#else
#  include <WProgram.h>
#endif

// include the library code:
#include <avr/power.h>
#include <LiquidCrystal.h>
#include <Wire.h>

#ifdef USE_ONEWIRE_TEMP
#include <OneWire.h>
#include <DallasTemperature.h>
#endif

#ifdef USE_MICROSD
#include "microsd.h"
#include <SdFat.h>
#endif

// Module constants
static const uint32_t VALID_POS_TIMEOUT = 2000;  // ms

// Module variables

#ifdef KILL_USB_TIME
unsigned long programStartTime = 0;
bool usb_alive                 = true;
#endif

static int32_t next_aprs = 0;

// Is the radio enabled?
bool radioEnabled = false;

// initialize the library with the numbers of the interface pins
#ifdef LCD_ENABLED
LiquidCrystal lcd(LCD_PINS);
#endif

#define DEBUG_SERIAL Serial

// Module variables for microSD support
#ifdef USE_MICROSD
  SdFat  sd;                                     // File system object
  SdFile logFile;                                // Log file
#endif

void setup()
{
#ifdef KILL_USB_TIME
  programStartTime = millis();
#endif

  pinMode(LED_PIN, OUTPUT);
  pin_write(LED_PIN, LOW);

  // Another hack because of the bootloader.  Remove these (and their definitions)
  // Once the leoTracker bootloader is fixed correctly.  
  RX_LED_ON();
  TX_LED_ON();
  
  GPS_SERIAL.begin(GPS_BAUDRATE);

#if defined(__AVR_ATmega32U4__) && (defined(DEBUG_GPS) || defined(DEBUG_RESET) || defined(DEBUG_MODEM) || defined(DEBUG_SENS) || defined(DEBUG_AX25))
  DEBUG_SERIAL.begin(115200);
  // Wait for serial port to connect. Needed for Atmega 32u4 processors only
  while (!DEBUG_SERIAL) {}
#endif

#ifdef DEBUG_RESET
  DEBUG_SERIAL.println("RESET");
#endif

#ifndef GPS_DISABLED
  buzzer_setup();
#endif
  afsk_setup();
#ifndef GPS_DISABLED
  gps_setup();
#endif
  sensors_setup();

#ifdef USE_MICROSD
  microsd_setup();
#endif  
  
#ifdef DEBUG_SENS
#ifdef USE_ONEWIRE_TEMP
#ifndef INTERNAL_DS18B20_DISABLED
  DEBUG_SERIAL.print("Ti=");
  DEBUG_SERIAL.print(sensors_int_ds18b20());
  DEBUG_SERIAL.print(", ");
#endif
#ifndef EXTERNAL_DS18B20_DISABLED
  DEBUG_SERIAL.print("Te=");
  DEBUG_SERIAL.print(sensors_ext_ds18b20());
  DEBUG_SERIAL.print(", ");
#endif
#else
#ifndef INTERNAL_LM60_DISABLED
  DEBUG_SERIAL.print("Ti=");
  DEBUG_SERIAL.print(sensors_int_lm60());
  DEBUG_SERIAL.print(", ");
#endif
#ifndef EXTERNAL_LM60_DISABLED
  DEBUG_SERIAL.print("Te=");
  DEBUG_SERIAL.print(sensors_ext_lm60());
  DEBUG_SERIAL.print(", ");
#endif
#endif
  DEBUG_SERIAL.print("Vin=");
  DEBUG_SERIAL.println(sensors_vin());
#endif

#ifndef GPS_DISABLED
  // Do not start until we get a valid time reference
  // for slotted transmissions.
  if (APRS_SLOT >= 0) {
    do {
      while (! GPS_SERIAL.available())
        power_save();
    } while (! gps_decode(GPS_SERIAL.read()));
    
    next_aprs = millis() + 1000 *
      (APRS_PERIOD - (gps_seconds + APRS_PERIOD - APRS_SLOT) % APRS_PERIOD);
  }
  else {
    next_aprs = millis();
  }
#else
  next_aprs = millis();
#endif
  // TODO: beep while we get a fix, maybe indicating the number of
  // visible satellites by a series of short beeps?
  
#ifdef LCD_ENABLED
  // set up the LCD's number of columns and rows: 
  lcd.begin(16, 2);
  // Print a message to the LCD.
  lcd.print(S_CALLSIGN);
  lcd.print("-");
  lcd.print(S_CALLSIGN_ID);
#endif
}

void get_pos()
{
  // Get a valid position from the GPS
  int valid_pos = 0;
  uint32_t timeout = millis();
  do {
    if (GPS_SERIAL.available())
      valid_pos = gps_decode(GPS_SERIAL.read());
  } while ( (millis() - timeout < VALID_POS_TIMEOUT) && ! valid_pos) ;

#ifndef GPS_DISABLED
  if (valid_pos) {
    if (gps_altitude > BUZZER_ALTITUDE) {
      buzzer_off();   // In space, no one can hear you buzz
    } else {
      buzzer_on();
    }
  }
#endif
}

void loop()
{
#if defined (RADIO_WARMUP_TIME) 
  // Time to power up the radio?
  if (((int32_t) (millis() - next_aprs + RADIO_WARMUP_TIME) >= 0) && !radioEnabled) {
    radioEnabled = true;
	afsk_enable(radioEnabled);

// NOTE: This is NOT good, if RADIO_WARMUP_TIME is not defined the OneWire bus
// will never get new temperatures...
#ifdef USE_ONEWIRE_TEMP
      request_temperatures();
#endif	  
  }
#endif 

  // Time for another APRS frame
  if ((int32_t) (millis() - next_aprs) >= 0) {
#ifndef GPS_DISABLED
    get_pos();
#endif
#ifdef LCD_ENABLED
    lcd.setCursor(0, 1);
#ifdef USE_ONEWIRE_TEMP
#ifndef INTERNAL_DS18B20_DISABLED
    lcd.print("Ti=, ");
    lcd.print(sensors_int_ds18b20());
#endif
#ifndef EXTERNAL_DS18B20_DISABLED
    lcd.print("Te=, ");
    lcd.print(sensors_ext_ds18b20());
#endif
#else	
#ifndef INTERNAL_LM60_DIABLED
    lcd.print("Ti=, ");
    lcd.print(sensors_int_lm60());
#endif
#ifndef EXTERNAL_LM60_DISABLED
    lcd.print("Te=, ");
    lcd.print(sensors_ext_lm60());
#endif
#endif
    lcd.print("Vin=");
    lcd.print(sensors_vin());
#endif
    aprs_send();
    next_aprs += APRS_PERIOD * 1000L;
    while (afsk_flush()) {
      power_save();
    }

#ifdef RADIO_WARMUP_TIME
    afsk_enable(false);
	radioEnabled = false;
#endif
	
#ifdef DEBUG_MODEM
    // Show modem ISR stats from the previous transmission
    afsk_debug();
#endif
  }

#ifdef KILL_USB_TIME
 if (((millis() - KILL_USB_TIME) >= 0) && usb_alive) {
   // Kill power to the USB after we've been running for more than 10 minutes
   // (just to be more power friendly)
   usb_alive = false;   
   power_usb_disable();
   }
#endif
  
  power_save(); // Incoming GPS data or interrupts will wake us up
}
