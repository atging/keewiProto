#include "SoftwareSerial.h"

#define SSID        "ATTFBzeiBA"
#define PASSWORD    "btvzks3g48=s"
#define WiFiResetPin 11
#define TIMEOUT     6000 // mS
#define CONTINUE    false
#define HALT        true
//ESP8266 wifi(Serial1, 115200); //some of the modules use 9600, some use 115200. If the WIFI doesn't work, try the other value first!

#define DEST_HOST   "ec2-54-197-129-252.compute-1.amazonaws.com"
#define DEST_IP     "54.197.129.252"
#define RESOURCE    "/keewi/api.php?action=GetStatus&user_id=11"
#define HOST_PORT   80
#define CHECKFREQUENCY  15000  // mS on/off status check frequency
// not including 2 second delay in TCP connection

// change these to float to be accurate at sub-int level
int V_zero = 0;
int I_zero = 0;
int Ilow_zero = 0;
bool switchStatus = false;
float Ilow_cal  = 0.0023; // amp/DV
float I_cal     = 0.0625; // amp/DV
float V_cal     = 1.123;  // volts/DV

int Triac      = 7;
int LedBlue    = 8;
int LedGreen   = 9;
int LedRed     = 10;
int V_Sense    = A0;
int I_Sense    = A1;
int I_SenseLow = A2;

// #define ECHO_COMMANDS // Un-comment to echo AT+ commands to serial monitor

// Print error message and loop stop.
void errorHalt(String msg)
{
  Serial.println(msg);
  Serial.println("HALT");
  while(true){};
}

// Read characters from WiFi module and echo to serial until keyword occurs or timeout.
boolean echoFind(String keyword)
{
  byte current_char   = 0;
  byte keyword_length = keyword.length();
  
  // Fail if the target string has not been sent by deadline.
  long deadline = millis() + TIMEOUT;
  while(millis() < deadline)
  {
    if (Serial1.available())
    {
      char ch = Serial1.read();
      Serial.write(ch);
      if (ch == keyword[current_char])
        if (++current_char == keyword_length)
        {
          Serial.println();
          return true;
        }
    }
  }
  return false;  // Timed out
}

// Read data from triac status API and return true or false for switch state (on or off)
int echoOnOffState(String keyword)
{
  byte current_char   = 0;
  byte keyword_length = keyword.length();
  int  onOffValue     = 0;
  char myregister[10];
  int  count          = 0;
  
  // Fail if the target string has not been sent by deadline.
  long deadline = millis() + TIMEOUT;
  while(millis() < deadline)
  {
    if (Serial1.available())
    {
      char ch = Serial1.read();
//      Serial.write(ch);
      if (count < keyword_length)
      {
        myregister[count] = ch;
        count++;
      }
      else
      {
        for (uint8_t i=0; i < count; i++)
        {
          myregister[i] = myregister[i+1];
//          Serial.write(myregister[i]);
        }
        myregister[count] = ch;
//        Serial.write(myregister[count]);
      }
      if (ch == keyword[current_char])
      {
        if (++current_char == keyword_length)  // last char in keyword
        {
//          Serial.println();
          // myregister[0] should hold the # in "#CLOSED", whether 0 or 1
          onOffValue = myregister[0] - '0';  // convert to integer
          return onOffValue;
        }
      }
      else
      {
        current_char = 0;  // prevent from false triggering start of keyword grab
        if (ch == keyword[current_char])  // need to immediately retry in case
        {                                 // this char is the start of keyword
          if (++current_char == keyword_length)  // last char in keyword
          {
//            Serial.println();
            // myregister[0] should hold the # in "#CLOSED", whether 0 or 1
            onOffValue = myregister[0] - '0';  // convert to integer
            return onOffValue;
          }
        }
      }
    } // endif Serial1.available
  }
  return -1;  // Timed out
}

// Read and echo all available module output.
// (Used when we're indifferent to "OK" vs. "no change" responses or to get around firmware bugs.)
void echoFlush()
  {while(Serial1.available()) Serial.write(Serial1.read());}
  
// Echo module output until 3 newlines encountered.
// (Used when we're indifferent to "OK" vs. "no change" responses.)
void echoSkip()
{
  echoFind("\n");        // Search for nl at end of command echo
  echoFind("\n");        // Search for 2nd nl at end of response.
  echoFind("\n");        // Search for 3rd nl at end of blank line.
}

// Send a command to the module and wait for acknowledgement string
// (or flush module output if no ack specified).
// Echoes all data received to the serial monitor.
boolean echoCommand(String cmd, String ack, boolean halt_on_fail)
{
  Serial1.println(cmd);
  #ifdef ECHO_COMMANDS
    Serial.print("--"); Serial.println(cmd);
  #endif
  
  // If no ack response specified, skip all available module output.
  if (ack == "")
    echoSkip();
  else
    // Otherwise wait for ack.
    if (!echoFind(ack))          // timed out waiting for ack string 
      if (halt_on_fail)
        errorHalt(cmd+" failed");// Critical failure halt.
      else
        return false;            // Let the caller handle it.
  return true;                   // ack blank or ack found
}

// Connect to the specified wireless network.
boolean connectWiFi()
{
  String cmd = "AT+CWJAP=\""; cmd += SSID; cmd += "\",\""; cmd += PASSWORD; cmd += "\"";
  if (echoCommand(cmd, "OK", CONTINUE)) // Join Access Point
  {
    Serial.println("Connected to WiFi.");
    return true;
  }
  else
  {
    Serial.println("Connection to WiFi failed.");
    return false;
  }
}

// ******** SETUP ********
void setup()  
{
  pinMode(Triac, OUTPUT);     
  pinMode(LedRed, OUTPUT); 
  pinMode(LedGreen, OUTPUT); 
  pinMode(LedBlue, OUTPUT); 
  
  pinMode(V_Sense,INPUT);
  pinMode(I_Sense,INPUT);
  pinMode(I_SenseLow,INPUT);
  
  Serial.begin(115200);         // Communication with PC monitor via USB
  Serial1.begin(115200);        // Communication with ESP8266 via TX/RX
  
  Serial1.setTimeout(TIMEOUT);
  Serial.println("ESP8266 Demo");

  // Determine zero-crossing values of V, I and Ilow
  // TODO: move this to a subroutine that returns the 3 zero numbers
  Serial.println("Averaging ADC samples to determine zero-crossing values.");
  int counter = 0;
  uint32_t V_sum = 0;  // need more than 16 bit cuz sum goes up to ~2.5million
  uint32_t I_sum = 0;
  uint32_t Ilow_sum = 0;
  while(counter<5000){
    V_sum += analogRead(V_Sense);
    I_sum += analogRead(I_Sense);
    Ilow_sum += analogRead(I_SenseLow);
    counter++;
    delayMicroseconds(100);
  }

  V_zero = V_sum / counter;
  I_zero = I_sum / counter;
  Ilow_zero = Ilow_sum / counter;
  Serial.print("Vzero = ");
  Serial.print(V_zero);
  Serial.print(", Izero = ");
  Serial.print(I_zero);
  Serial.print(", Ilowzero = ");
  Serial.print(Ilow_zero);

  pinMode(WiFiResetPin, OUTPUT);
  digitalWrite(WiFiResetPin, HIGH);
  
  delay(2000);

  echoCommand("AT+RST", "ready", HALT);    // Reset & test if the module is ready  
  Serial.println("Module is ready.");
  echoCommand("AT+GMR", "OK", CONTINUE);   // Retrieves the firmware ID (version number) of the module. 
  echoCommand("AT+CWMODE?","OK", CONTINUE);// Get module access mode. 
  
  // echoCommand("AT+CWLAP", "OK", CONTINUE); // List available access points - DOESN't WORK FOR ME
  
  echoCommand("AT+CWMODE=1", "", HALT);    // Station mode
  echoCommand("AT+CIPMUX=0", "", HALT);    // Allow single connection only, no multiplexing

  //connect to the wifi
  boolean connection_established = false;
  for(uint8_t i=0;i<5;i++)
  {
    if(connectWiFi())
    {
      connection_established = true;
      break;
    }
  }
  if (!connection_established) errorHalt("Connection failed");

  delay(5000);

  //echoCommand("AT+CWSAP=?", "OK", CONTINUE); // Test connection
  echoCommand("AT+CIFSR", "", HALT);         // Echo IP address. (Firmware bug - should return "OK".)
  //echoCommand("AT+CIPMUX=0", "", HALT);      // Set single connection mode  

}

// ******** LOOP ********
void loop() 
{ 
  Serial.println("Beginning ADC Test.");
  if (switchStatus)
  {
    digitalWrite(Triac, LOW);
    switchStatus = false;
  }
  else
  {
    digitalWrite(Triac, HIGH);
    switchStatus = true;
  }
  
  float V_value = 0.0, V_sum = 0.0, rms_voltage = 0.0;
  float I_value = 0.0, I_sum = 0.0, rms_current = 0.0;
  float Ilow_value = 0.0, Ilow_sum = 0.0, rms_Ilow = 0.0;
  unsigned long n = 0;
  unsigned long now = millis() + 100;
  while(millis()<now){
    V_value = analogRead(V_Sense) - V_zero;
    I_value = analogRead(I_Sense) - I_zero;
    Ilow_value = analogRead(I_SenseLow) - Ilow_zero;
    Serial.println(Ilow_value);
    V_value *= V_value; // square
    V_sum += V_value;
    I_value *= I_value; // square
    I_sum += I_value;
    Ilow_value *= Ilow_value;
    Ilow_sum += Ilow_value;
    delayMicroseconds(1);
    n++;
  }  
  rms_voltage = sqrt(V_sum / n);
  Serial.print("VRMS = ");
  Serial.print(rms_voltage);
  Serial.print(" (DV), volts = ");
  Serial.println(rms_voltage * V_cal);
  rms_current = sqrt(I_sum / n);
  Serial.print("IRMS = ");
  Serial.print(rms_current);
  Serial.print(" (DV), amps = ");
  Serial.println(rms_current * I_cal);
  rms_Ilow = sqrt(Ilow_sum / n);
  Serial.print("IlowRMS = ");
  Serial.print(rms_Ilow);
  Serial.print(" (DV), amps = ");
  Serial.println(rms_Ilow * Ilow_cal);

  String cmd;
  // Establish TCP connection
  cmd = "AT+CIPSTART=\"TCP\",\""; cmd += DEST_IP; cmd += "\",80";
  if (!echoCommand(cmd, "OK", CONTINUE)) return;
  delay(2000);
  
  // Get connection status 
  if (!echoCommand("AT+CIPSTATUS", "OK", CONTINUE)) return;

  // Build HTTP request.
  cmd = "GET "; cmd += RESOURCE; cmd += " HTTP/1.1\r\nHost: "; cmd += DEST_HOST; cmd += ":80\r\n\r\n";
  
  // Ready the module to receive raw data
  if (!echoCommand("AT+CIPSEND="+String(cmd.length()), ">", CONTINUE))
  {
    echoCommand("AT+CIPCLOSE", "", CONTINUE);
    Serial.println("Connection timeout.");
    return;
  }
  
  // Send the raw HTTP request
  echoCommand(cmd, "OK", CONTINUE);  // GET

//  String  data;
  int     statusValue;
  String  out = "statusValue = ";
  
  // Loop forever echoing data received from destination server.
  while (Serial1.available())
  {  // Need the integer (0 or 1) immediately prior to this: "0,CLOSED"  
    statusValue = echoOnOffState("CLOSED");
    if (statusValue == 1)
      digitalWrite(Triac, HIGH);
    else if (statusValue == 0)
      digitalWrite(Triac, LOW);
      
    out += String(statusValue);
    Serial.println(out);
    while (Serial1.available())
    {
      Serial.write(Serial1.read());
    }
  }
  
  delay(CHECKFREQUENCY);
//  errorHalt("ONCE ONLY");
}
