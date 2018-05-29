/*
  Current version of the code for the docking station prototype
  Use OpenscaleOriginal to determine the calibration values of
  the load cell you're using, then enter them below
  The load cell specific values are marked with an Ø
  Use Read_Write_1k_Mifare to configue your nfc chip
  It must be a 1k Mifare chip
*/

#include "HX711.h" //Original Repository Created by Bodge https://github.com/bogde/HX711
#include "openscale.h" //Contains EPPROM locations for settings

//#include <Wire.h> //Needed to talk to on board TMP102 temp sensor
//#include <EEPROM.h> //Needed to record user settings
//#include "OneWire.h" //Needed to read DS18B20 temp sensors

#include <avr/sleep.h> //Needed for sleep_mode
#include <avr/power.h> //Needed for powering down perihperals such as the ADC/TWI and Timers

#include <SPI.h>
#include <MFRC522.h>

constexpr uint8_t RST_PIN = 9;     // Configurable, see typical pin layout above
constexpr uint8_t SS_PIN = 4;     // Configurable, see typical pin layout above

#define FIRMWARE_VERSION "1.0"

const byte statusLED = 13;  //Flashes with each reading

byte block;
byte len;

MFRC522 mfrc522(SS_PIN, RST_PIN);   // Create MFRC522 instance

const int RATE = 60;
float flowrate[RATE];
int flowrateCycle = 0;
int error = 0;
float displayMinuteFlowrate1 = 0;
int displayMinuteFlowrate = 0;
boolean displayTheFlowrate = false;
float previousReading = 0;
float flowrate_reading, temp_reading = 0, temp_flowrate = 0;
float previousMinuteReading = 0, minuteFlowrate = 0;
byte setting_flowrate;
int count = 0, count1 = 0, average = 0;
long timeConfigure = 0;

//these need to be changed depending on your load cell
long setting_calibration_factor = 630815; // Ø Value used to convert the load cell
long setting_tare_point = 203451; // Ø Zero value that is found when scale is tared

unsigned int setting_report_rate = 1000; //1 second, dont change
long setting_uart_speed = 9600; //You must use 9600 for the android app
byte setting_decimal_places = 4; //How many decimals to display
byte setting_average_amount = 4; //How many readings to take before reporting reading
boolean setting_status_enable = 1; //Turns on/off the blinking status LED

const int minimum_powercycle_time = 500; //Anything less than 500 can cause reading problems

HX711 scale(DAT, CLK); //Setup interface to scale

bool nfcRead = false;

void setup()
{
  pinMode(statusLED, OUTPUT);

  //Power down various bits of hardware to lower power usage
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();

  SPI.begin(); // Init SPI bus
  mfrc522.PCD_Init();  // Init MFRC522 card

  //Shut off Timer2, Timer1, ADC
  ADCSRA &= ~(1 << ADEN); //Disable ADC
  ACSR = (1 << ACD); //Disable the analog comparator
  DIDR0 = 0x3F; //Disable digital input buffers on all ADC0-ADC5 pins
  DIDR1 = (1 << AIN1D) | (1 << AIN0D); //Disable digital input buffer on AIN1/0

  power_timer1_disable();
  power_timer2_disable();
  power_adc_disable();
  //power_spi_disable(); //Uncomment this if nfc is unused

  Serial.begin(setting_uart_speed);

  //checkEmergencyReset(); //Look to see if the RX pin is being pulled low

  scale.set_scale(setting_calibration_factor);
  scale.set_offset(setting_tare_point);

  powerDownScale(); //remove these if you want to use the load cell before nfc
  power_timer0_disable();
  power_twi_disable();
}

void loop()
{
  //use these functions to control when and how many times you want nfc to be read
  //currently it waits for an nfc read once, then stays in loadCell()
  //while (nfcRead) loadCell();
  loadCell();

  //once checkNFC() is called, it will only exit when an nfc tag is scanned
  //checkNFC();
}

void loadCell()
{
  power_timer0_enable();
  power_twi_enable();
  powerUpScale();
  //Power cycle takes around 400ms so only do so if our report rate is greater than 500ms
  if (setting_report_rate > minimum_powercycle_time) powerUpScale();

  long startTime = millis();
  if (timeConfigure == 0) timeConfigure = millis(); //this makes sure the time starts at 0

  //Take average of readings with calibration and tare taken into account
  float currentReading = scale.get_units(setting_average_amount);
  currentReading *= -1; //This is for a negative reading. Swap the green and white wires for a better solution

  Serial.print(F("#")); //start of string

  //Print calibrated reading
  float displayReading = currentReading * 1000;
  leadingZeros((int)displayReading, 4); //4 is for the amount of zeros
  if ((int)displayReading >= 1)
    Serial.print(displayReading, 0); //this is to stop 5 zeros printing
  Serial.print(F("&mls*"));

  Serial.print(F("@")); //these do nothing. if you want to remove them, you will need to change the android code accordingly

  if (displayTheFlowrate == true)
  {
    //displayMinuteFlowrate1 = (((flowrate[flowrateCycle] - currentReading) * 60) * 1000) - error;
    displayMinuteFlowrate = (((flowrate[flowrateCycle] - currentReading) * 1000) * 60) - error;
    if (displayMinuteFlowrate >= 1 && displayMinuteFlowrate < 9999)
    {
      Serial.print(F(","));
      leadingZeros(displayMinuteFlowrate, 4);
      Serial.print(displayMinuteFlowrate);
      previousMinuteReading = displayMinuteFlowrate;
    }
    else Serial.print(F(",0000"));
  }
  else Serial.print(F(",0000"));

  //this flashes the blue Led on the board to show the code is running
  if (digitalRead(statusLED)) digitalWrite(statusLED, HIGH);
  else digitalWrite(statusLED, LOW);

  flowrate[flowrateCycle] = currentReading;

  if (flowrateCycle < RATE - 1) flowrateCycle++;
  else
  {
    flowrateCycle = 0;
    displayTheFlowrate = true;
  }

  int currentTime = (startTime - timeConfigure) / 1000;
  Serial.print(F(","));
  leadingZeros(currentTime, 7);
  if (currentTime > 0) Serial.print(currentTime);

  Serial.print(F("~")); //end of string
  Serial.println();
  Serial.flush();

  //This takes time so put it after we have printed the report
  if (setting_report_rate > minimum_powercycle_time) powerDownScale();

  //Hang out until the end of this report period
  while (1)
  {
    if (Serial.available())
    {
      //This part deals with incoming characters to enable
      //comminication from the app.
      //I've left these ifs here as example
      char incoming = Serial.read();
      //if (incoming == '0') toggleLED();
      //if ( incoming == 'l' || incoming == 'L') digitalWrite(onLED, HIGH);
    }

    if ((millis() - startTime) >= setting_report_rate) break;
  }
}

void checkNFC()
{
  // Prepare key - all keys are set to FFFFFFFFFFFFh at chip delivery from the factory.
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  MFRC522::StatusCode status;

  // Look for new cards
  if ( ! mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Select one of the cards
  if ( ! mfrc522.PICC_ReadCardSerial()) {
    return;
  }
  //code will not pass here if an nfc chip isnt present
  nfcRead = true; //you can change this bool to an increment for multiple nfc reads

  int words = 7; //choose between 1 and 16 depending on how many words you saved

  Serial.print(F(""));

  for (int j = 0; j < words; j++)
  {
    block = 1 + (j * 4);
    len = 18;
    byte buffer1[len];

    status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid)); //line 834 of MFRC522.cpp file
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("Authentication failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return;
    }

    status = mfrc522.MIFARE_Read(block, buffer1, &len);
    if (status != MFRC522::STATUS_OK) {
      Serial.print(F("Reading failed: "));
      Serial.println(mfrc522.GetStatusCodeName(status));
      return;
    }
    
    //PRINT
    for (uint8_t i = 0; i < 16; i++)
    {
      if (buffer1[i] != 32)
      {
        Serial.write(buffer1[i]);
      }

    }
    Serial.print(F("/"));
  }
  Serial.println(F("~"));
  Serial.println();

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  Serial.flush();
}

//this prints zeros to keep a consistant string length
void leadingZeros(int flow, int zeros)
{
  for (int t = 0; t < zeros; t++)
  {
    if (flow < pow(10, t))
      Serial.print(B0);
  }
}

void powerUpScale(void)
{
  scale.power_up();
}

void powerDownScale(void)
{
  scale.power_down();
}

//I left this function in as it seemed important, best not remove it
//Check to see if we need an emergency UART reset
//Scan the RX pin for 2 seconds
//If it's low the entire time, then return 1
void checkEmergencyReset(void)
{
  pinMode(0, INPUT); //Turn the RX pin into an input
  digitalWrite(0, HIGH); //Push a 1 onto RX pin to enable internal pull-up

  //Quick pin check
  if (digitalRead(0) == HIGH) return;

  Serial.println(F("Reset!"));

  //Wait 2 seconds, blinking LED while we wait
  pinMode(statusLED, OUTPUT);
  digitalWrite(statusLED, LOW); //Set the STAT1 LED

  for (byte i = 0 ; i < 80 ; i++)
  {
    delay(25);

    //toggleLED();

    if (digitalRead(0) == HIGH) return; //Check to see if RX is not low anymore
  }

  //If we make it here, then RX pin stayed low the whole time
  //set_default_settings(); //Reset baud, escape characters, escape number, system mode

  //Now sit in forever loop indicating system is now at 9600bps
  while (1)
  {
    delay(1000);
    //toggleLED();
    Serial.println(F("Reset - please power cycle"));
  }
}


