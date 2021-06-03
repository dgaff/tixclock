#include "NTPClient.h"
#include "ESP8266WiFi.h"
#include "WiFiUdp.h"
#include "TimeLib.h"
#include "Timezone.h"
#include "GDBStub.h"
#include "Ticker.h"

// Don't save your wifi to GitHub! Put these two declarations in your wifisecrets.h file and replace with your SSID and PW.
// const char *ssid     = "SSID";
// const char *password = "WIFI Password";
#include "wifisecrets.h"

// Wemos pin mapping
// #define A0 not sure  ADC0
// Digital pins can supply 12 mA max, so use transistor to switch higher current
#define D0 16 // WAKE
#define D1 5  // SCL
#define D2 4  // SDA
#define D3 0  // FLASH
#define D4 2  // Blue LED - lights on digital 0
#define D5 14 // SCLK
#define D6 12 // MISO
#define D7 13 // MOSI
#define D8 15 // CS
#define TX 1  // TX
#define RX 3  // RX

// Timezone rules and initialization
TimeChangeRule usEDT = {"EDT", Second, Sun, Mar, 2, -240};  //UTC - 4 hours
TimeChangeRule usEST = {"EST", First, Sun, Nov, 2, -300};   //UTC - 5 hours
Timezone usEastern(usEDT, usEST);

// Turn on serial output for debugging
//#define SERIAL

// Time info. I'm using timezone library to convert, so no UTC offset required.
//const long utcOffsetInSeconds = -18000; // EST
//const long utcOffsetInSeconds = -14400; // EDT
const long utcOffsetInSeconds = 0; // UTC

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "north-america.pool.ntp.org", utcOffsetInSeconds);

// VT100 Terminal Emulation - helpful for serial debug output
#define OFF         "\033[0m"
#define BOLD        "\033[1m"
#define LOWINTENS   "\033[2m"
#define UNDERLINE   "\033[4m"
#define BLINK       "\033[5m"
#define REVERSE     "\033[7m"
#define GOTOXY( x,y) "\033[x;yH"   // Esc[Line;ColumnH  <-- this doesn't seem to be expanding correclty
#define CLS          "\033[2J"

/*
 TIX Clock Notes
 
 Upon power up, clock starts at 12:34 for 12-hour mode.
 If you hold Mode in during power up, clock starts at 21:34 for 24-hour mode.

 The clock immediately starts counting seconds, but setting the time as explained below retarts
 the second hand.

 Mode
 - Hold on power up for 24-hour mode.
 - Hold for 3 seconds to select three different update modes. Then cycle through the modes using Increment.
    (Startup) 4 sec update, Left LED 2 lit
    60 sec update, Left LED 3 lit
    1 sec update, Left LED 1 lit
 - Click to advance between hours, minute first digit, minute second digit. Then use Increment to select time.
   One done setting time, click mode again. This starts the second hand at HH:MM:00.

 Increment 
 - Sets brightness to one of three levels: High (startup), Medium, Low
 - Advances digits or modes in conjuction with digital selection using Mode

The clock gets its LED update rate from a input pin to the PIC chip, which takes a 60Hz AC signal. 
This is faked out below with a ~60Hz digital pulse train on D4 using an ISR.

I _think_ the clock gets its time reference from the internal oscillator on the PIC chip, but
I don't know for sure yet.

D1 is Mode: HIGH (open drain) == Off, LOW == Pressed
D2 is Increment: HIGH (open drain) == Off, LOW == Pressed
D3 is clock power through transistor
D4 is 60 Hz pulse train for LED update rate
*/ 

// Set these for desired clock operating states.

bool clock24hrMode = true;
int clockbrightness = 1; // 4=medium (startup), 3=low, 2=dim, 1=high
int displayMode = 1; // 3=4sec (startup), 2=60sec, 1=1sec
bool nightModeOn = true;
int nightHour = 21; // time to shut off
int dayHour = 8;    // time to turn on, make sure it's >= 1

/*
  ISR timer routine to ensure we generate a constant 60 Hz pulse train. I tried bit banging in the main loop,
  but some of the clock setting functionality requires the pulse train to be active. Also, the ISR is more
  accurate.

  Credit: https://www.visualmicro.com/page/Timer-Interrupts-Explained.aspx

	Dividers:
		TIM_DIV1 = 0,   //80MHz (80 ticks/us - 104857.588 us max)
		TIM_DIV16 = 1,  //5MHz (5 ticks/us - 1677721.4 us max)
		TIM_DIV256 = 3  //312.5Khz (1 tick = 3.2us - 26843542.4 us max)
	Reloads:
		TIM_SINGLE	0 //on interrupt routine you need to write a new value to start the timer again
		TIM_LOOP	1   //on interrupt the counter will start with the same value again

  The ESP8266 is 5 MHz.

  noInterrupts() and interrupts() didn't seem to disable the timer interrupt for me, so there's a flag to turn the pulse
  train on and off. It's volatile in the hope that changing it is atomic with respect to the ISR. Usually you'd wrap a mutex
  around changing an ISR variable outside of the ISR. But from what I've read online, you just make the global variable 
  volatile. I'm skeptical, but the pulse train is only disabled on restarting the clock, so maybe it's not a concern.
*/

Ticker timer;
volatile bool pulseState = false;
volatile bool enablePulseOn = false; 

// ISR to Fire when Timer is triggered
void ICACHE_RAM_ATTR onTime() 
{
  // Pulse train is controlled by a volatile global variable.
  if (enablePulseOn)
  {
    if (pulseState) digitalWrite(D4, HIGH);
    else digitalWrite(D4, LOW);

    pulseState = !pulseState;
  }

	// Re-Arm the timer as using TIM_SINGLE. Could use TIM_LOOP instead, I think.
	timer1_write(41667);
}

// Once armed, the ISR will always fire. The pulse is controlled by the global variable.

void startACPulse()
{
  // This enables the pulse in the ISR
  enablePulseOn = true;

	// (re) Arm the Timer. 
  // 60 Hz = 16,666.667 us, but for the square wave, the interupt needs to fire twice as fast
  // to get one on and one off pulse per period.
  // 8,333.333 us * 5 ticks/us = 41667 ticks
	timer1_write(41667); 
}

// Interrupt will still fire, but it won't issue pulses.

void stopACPulse()
{
  enablePulseOn = false;
  delay(200);
}

// Disable the AC pulse and remove power to the clock. All LEDs turn off.

void clockOff()
{
  // Disable the AC pulse
  stopACPulse();

  // Turn off clock power
  digitalWrite(D3, LOW);
  delay(1000);
}

// Cycles through brightness values. Should be called after clock is reset.

void setBrightness()
{
  int brightness = 4; // startup value

  while (brightness > clockbrightness)
  {
    digitalWrite(D2, LOW);
    delay(100);
    digitalWrite(D2, HIGH);
    delay(100);
    brightness--;
  }
}

// Cycles through display update modes. Should be called after clock is reset.

void setDisplayMode()
{
    int mode = 3; // startup mode

    if (mode > displayMode)
    {
      // Hold Mode low for three seconds
      digitalWrite(D1, LOW);
      delay(3000);
      digitalWrite(D1, HIGH);
      delay(100);
 
      // Click Increment until desired mode is selected
      while (mode > displayMode)
      {
        digitalWrite(D2, LOW);
        delay(100);
        digitalWrite(D2, HIGH);
        delay(100);
        mode--;
      }

      // Click Mode to set
      digitalWrite(D1, LOW);
      delay(100);
      digitalWrite(D1, HIGH);
      delay(200);
    }
}

// Power cycles the clock.

void restartClock()
{
    // Turn off clock first
    clockOff(); 

    // Hold Mode low for 24-hour mode
    if (clock24hrMode) 
    {
      digitalWrite(D1, LOW);
      delay(200);
    }

    // Turn on clock and enable the AC pulse. Clock will show "TIX" until the pulse train starts.
    digitalWrite(D3, HIGH);
    startACPulse();
    delay(200); 

    // Release Mode for 24-hour mode
    if (clock24hrMode)
    { 
      digitalWrite(D1, HIGH);
      delay(200);
    } 

    // Give the clock a few seconds to startup
    delay(5000);
}

// This sets the clock time but does not exit time setup mode. Set the clock time several seconds before network time
// reaches :00. Then click the mode button to start seconds at :00. Should be called after restarting the clock.

void setClockTime(int hour, int minute)
{
    int hr = hour;
    int hrcount;

    // Clock starts at 12:34 or 21:34. Handle wrap-around when the clock hour has started past the current time.
    if (clock24hrMode) 
    {
      hrcount = 21;
      if (hr < 21) hr = hr + 24;
    }
    else 
    { 
      hrcount = 12;
      if (hr < 12) hr = hr + 24;
    }

    // Get minute digits. We have to advance each digit separately.
    int mindigit1 = minute / 10; // integer division will truncate the decimal
    int mindigit2 = minute - (mindigit1 * 10);

    // Clock starts at 34 minutes.
    int mincount1 = 3;
    int mincount2 = 4;

    // Handle wrap-around case when the clock minute has started past the current time.
    if (mindigit1 < 3) mindigit1 = mindigit1 + 6;
    if (mindigit2 < 4) mindigit2 = mindigit2 + 10;

    // Click Mode to set Hour
    digitalWrite(D1, LOW);
    delay(100); 
    digitalWrite(D1, HIGH);
    delay(500);

    // Advance the hour
    while (hrcount < hr) 
    {
      digitalWrite(D2, LOW);
      delay(50);
      digitalWrite(D2, HIGH);
      delay(50);
      hrcount++;
    }

    // Click Mode to set first digit of Minute
    digitalWrite(D1, LOW);
    delay(100); 
    digitalWrite(D1, HIGH);
    delay(500);

    // Advance the first digit of the minute
    while (mincount1 < mindigit1)
    {
      digitalWrite(D2, LOW);
      delay(50);
      digitalWrite(D2, HIGH);
      delay(50);
      mincount1++;
    }

    // Click Mode to set second digit of Minute
    digitalWrite(D1, LOW);
    delay(100); 
    digitalWrite(D1, HIGH);
    delay(500);

    // Advance the second digit of the minute
    while (mincount2 < mindigit2)
    {
      digitalWrite(D2, LOW);
      delay(50);
      digitalWrite(D2, HIGH);
      delay(50);
      mincount2++;
    }

    // At this point, one more click of Mode will start the clock at HH:MM:00
}

// Run once setup - sets up the initial output pin states and gets on wifi

void setup()
{
  // Serial port output. Note that GDB isn't working reliably yet.
#ifdef SERIAL  
  Serial.begin(115200);
//  gdbstub_init();
#endif

  // Set all pins to digital output
  // When unpressed (HIGH), leave Mode and Increment at 5V. Using OUTPUT instead of OUTPUT_OPEN_DRAIN
  // forces pin to 3.3V.
  pinMode(D0, OUTPUT); 
  pinMode(D1, OUTPUT_OPEN_DRAIN); 
  pinMode(D2, OUTPUT_OPEN_DRAIN); 
  pinMode(D3, OUTPUT);
  pinMode(D4, OUTPUT);
  pinMode(D5, OUTPUT);
  pinMode(D6, OUTPUT);
  pinMode(D7, OUTPUT);
  pinMode(D8, OUTPUT);

  // Mode/Increment buttons unpressed. Clock off. Pulse train off. 
  digitalWrite(D1, HIGH);
  digitalWrite(D2, HIGH);
  digitalWrite(D3, LOW);
  digitalWrite(D4, LOW);

	// Initialize ticker interrupt ISR function.
	timer1_attachInterrupt(onTime);
	timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);

  // The clock will be restarted when the main loop sets the time, but this just ensures
  // the clock lights upon power up. This also ensures a clean reset if you press the reset button.
  restartClock();

  // Connect to Wifi
  WiFi.begin(ssid, password);
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
  }

  // Initialize NTP time
  timeClient.begin();
}

// Test code to ensure 12 and 24 modes are set correctly. This is just to make sure I got the digit wraparound 
// correct since setting the time is a little screwy.

int testcasecount = 0;
unsigned long testInterval = 0;

void testloop()
{
  if (testcasecount == 0)
  {
    testInterval = millis();
    testcasecount++;
  }
  else if (testcasecount)
  {
    unsigned long testdelta = millis() - testInterval;

    if (testcasecount == 1 && testdelta > 8000)
    {
      clock24hrMode = false;
      restartClock();
      setClockTime(11, 11);

      testcasecount++;
      testInterval = millis();
    }
    else if (testcasecount == 2 && testdelta > 8000)
    {
      clock24hrMode = false;
      restartClock();
      setClockTime(15, 55);

      testcasecount++;
      testInterval = millis();
    }
    else if (testcasecount == 3 && testdelta > 8000)
    {
      clock24hrMode = true;
      restartClock();
      setClockTime(11, 11);

      testcasecount++;
      testInterval = millis();
    }
    else if (testcasecount == 4 && testdelta > 8000)
    {
      clock24hrMode = true;
      restartClock();
      setClockTime(23, 55);

      testcasecount=0;
      testInterval = millis();
    }
  }

  yield();
}

// Main Loop - primary job is to set the clock time once and watch for DST changes. May add clock drift support at some point.

int initClockTime = 1; // 1 = power cycle the clock at :30   2 = synchronize seconds and set the time at :00
bool isDST = false;    // used to catch DST change, not yet tested

void loop()
{
  // Read the UTC time
  timeClient.update();
  time_t timeUTC = timeClient.getEpochTime();

  // Convert to EST or EDT
  time_t time = usEastern.toLocal(timeUTC);

  // Macros to calculate local time
  int seconds = numberOfSeconds(time);
  int hour = numberOfHours(time);
  int minute = numberOfMinutes(time);

  // Do we need to set the LED clock? 
  if (initClockTime)
  {
    // Power cycle the clock ahead of second syncronization
    if (initClockTime == 1 && seconds == 30)
    {
      // Add 30 seconds to advance to the next minute. We have to set the clock to the time 
      // it will be when the second hand turns over.
      time_t adjustedtime = time + 30;
      int adjustedhour = numberOfHours(adjustedtime);
      int adjustedminute = numberOfMinutes(adjustedtime);

      // Clock startup sequence.
      restartClock();
      setBrightness();    
      setDisplayMode();
      setClockTime(adjustedhour, adjustedminute);

      // Next phase of clock sync.
      initClockTime++;
    }
    // Wait for seconds to roll around to zero. This will sync to NTP time, at least as close as we can get.
    else if (initClockTime == 2 && seconds == 0)
    {
#ifdef SERIAL
      char timestr[10];
//  Serial.print("\033[0;0H"); // terminal emulation to go to top
//  sprintf(timestr, "%02d:%02d:%02d", timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
      sprintf(timestr, "%02d:%02d:%02d", hour, minute, seconds);
      Serial.println(timestr);
#endif  

      // Since this is called 30 seconds after Hour and Minute were set, we just click Mode to start the second hand.
      digitalWrite(D1, LOW);
      delay(100);
      digitalWrite(D1, HIGH);
      delay(100);

      isDST = usEastern.locIsDST(time);
      initClockTime = 0;
    }
  }
  // Only check for DST or drift after clock is set.
  else
  {
    // Check for DST change once the clock has been initialized. isDST variable was set on the first initClockTime.
    if (isDST != usEastern.locIsDST(time)) initClockTime = 1;

    // Check for drift correction
    /*
      Need to watch the clock for a while and see how much it drifts. Then just periodically init the clock again.
      If the LED clock is put on a Hue timer or something to have it off at night, this isn't really necessary.  
    */

    // Check for night mode
    if (nightModeOn)
    {
      // Turn on for daytime, off for nightime
      if (hour == dayHour-1 && minute == 59 && seconds == 0) initClockTime = 1;
      else if (hour == nightHour && minute == 0 && seconds == 0) clockOff();
    }
  }

  yield();
}
