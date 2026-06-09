/***************************************************************************
  This sketch is used for controlling a Honeywell Chronotherm III thermostat.
  After receiving a command via email, it simulates button presses to set
  the thermostat to a new mode or to change the temperature setting.
  The sender of the command is notified of the results.
  Various information, such as the latest handled command, is presented on a
  web page. 

  This sketch uses code from the following websites:
  https://randomnerdtutorials.com/esp8266-web-server/
  https://www.arduino.cc/en/Tutorial/UdpNTPClient
  https://github.com/sfrwmaker/sunMoon
  https://github.com/mobizt/ReadyMail

  Written by Tsjakka from the Netherlands.
  BSD license, this line and all text above must be included in any redistribution.
 ***************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Time.h>
#include <TimeLib.h>
#include <sunMoon.h>
#include <Preferences.h>
#include <Secrets.h>

#define ENABLE_SMTP
#define ENABLE_IMAP
#define ENABLE_DEBUG
#include <ReadyMail.h>

// ************************************************************************
// Create file Secrets.h with the following contents. Replace with your
// information where indicated.
// ************************************************************************
/*
// Wifi connection
const char* ssid = "REPLACE_WITH_YOUR_SSID";
const char* password = "REPLACE_WITH_YOUR_PASSWORD";

// Email
const char* fromAddress = "REPLACE_WITH_EMAIL_ADDRESS";       // The email address you want messages to be sent from
const char* toAddress = "REPLACE_WITH_EMAIL_ADDRESS";         // The email address you want messages sent to
const char* smtpServer = "REPLACE_WITH_SERVER_URL";           // The server to use for sending email (e.g. smtp.gmail.com)
const char* imapServer = "REPLACE_WITH_SERVER_URL";           // The server to use for reading email (e.g. imap.gmail.com)
const char* emailAccount = "REPLACE_WITH_EMAIL_ADDRESS";      // The email account on the server (e.g. myaddress@gmail.com)
const char* emailPassword = "REPLACE_WITH_PASSWORD";          // The password for the email account (for Gmail an App Password)

// Location
const float Latitude = 0.0000000;                             // Replace with your coordinates
const float Longitude = 0.000000;
*/
// ************************************************************************
// End of Secrets.h
// ************************************************************************

// ************************************************************************
// Change the constants below to adjust the software to your situation
// ************************************************************************
// Regional settings
uint8_t Timezone = 60;                      // UTC difference in minutes (can be changed through web page)
bool UseDST = true;                         // Is Daylight Saving Time observed in your region (can be changed through web page)

// Button commands (GPIO pins)
const int START_PROGRAM = 0;
const int HOLD_TEMP = 1;
const int WARMER = 3;
const int COOLER = 4;
// ************************************************************************
// Change the constants above to adjust the software to your situation
// ************************************************************************

// Timing
const int HOLD_TIME = 300;   // ms to hold a keypress
const int IDLE_TIME = 600;   // ms between keypresses

// Temperature limits of Chronotherm III
const int MIN_TEMP = 7;
const int MAX_TEMP = 20;

#define RETRY_PERIOD 30                     // Number of seconds before retrying wifi connection
#define IDLE_MODE false
#define AWAIT_MODE true
#define MAX_CONTENT_SIZE 1024 * 1024        // Maximum size in bytes of the body parts (text and attachment) to be downloaded.

#define ARDUINO_LOOP_STACK_SIZE (8 * 1024)

#define MAX_COMMANDS 10                     // Maximum number of commands to store

Preferences preferences;                    // Instance of the Preferences library

// WiFi stuff
const time_t connectPeriod = 90;            // Max period (in s) for setting up a wifi connection
uint8_t retryPeriod = RETRY_PERIOD;         // Number of seconds before retrying wifi connection

// NTP stuff
const char* NtpServer = "pool.ntp.org";     // A pool of NTP servers
const unsigned int localPort = 2390;        // Local port to listen for UDP packets
const int NtpPacketSize = 48;               // NTP time stamp is in the first 48 bytes of the message
const time_t UpdateTimeTimeout = 300;       // Time (in seconds) we will try updating the clock
WiFiUDP Udp;                                // A UDP instance for sending and receiving packets over UDP
byte packetBuffer[NtpPacketSize];           // Buffer to hold incoming and outgoing packets
bool ntpTimeInitialized = false;            // Has the time been set at least once through NTP?
bool ntpTimeSet = false;                    // Has the time been set through NTP?
bool emailSent = false;                     // Has an email been sent because setting the time failed?
int lastSecond = -1;                        // The second we last did stuff for NTP

// For reading mail
WiFiClientSecure imap_client;
IMAPClient imap(imap_client);
const time_t LoopPeriod = 180;              // Number of seconds between checks
std::vector<uint32_t> msgsToDelete;         // Global or static list of messages to delete
time_t imapUpdateStarted = 0;               // The time when we last checked for email
time_t imapReconnectAt = 0;                 // The earliest time to retry connecting to IMAP
const time_t ReadMailFrequency = 300;       // Interval for reading email
const time_t ImapReconnectFrequency = 60;   // Interval for retrying a dropped IMAP connection
struct PendingMailCommand {
  uint32_t msgNum;
  String subject;
};
std::vector<PendingMailCommand> pendingMailCommands;  // List of commands read from the retrieved emails

// For sending mail
WiFiClientSecure smtp_client;
SMTPClient smtp(smtp_client);

// Set web server port number to 80
WiFiServer server(80);
WiFiClient client;
bool clientActive = false;                  // A web client is active
String currentLine = "";                    // A string to hold incoming data from the client
time_t clientConnectedAt;
const time_t webConnectPeriod = 30;         // Max timeout period (in s) for HTTP connections
String header;                              // Variable to store the HTTP request

time_t clockUpdateStarted;                  // The time when we started updating the clock
bool settingSunRiseSunSet = false;          // True when setting the sunrise and sunset

// Daylight Saving Time (0 or 60 minutes, calculated from current date)
int DST = 0;

// For sunrise and sunset
sunMoon sm;
time_t sunRise = 0;
time_t sunSet = 0;

#define MAX_DEBUG_TEXT 16000
String debugText = "";                      // A string to hold all debug text

// Commands
struct HeatCommand {
  int action;         // 0 for OFF, 1 for ON (default), 2 for STATUS, 3 for REMOVE
  int temperature;    // -1 if not provided
  String date;        // Empty if not provided
  String time;        // Empty if not provided
};

HeatCommand commands[MAX_COMMANDS];
int commandCount = 0;

enum WebCommand {
  DownCmd,
  UpCmd,
  HoldCmd,
  StartCmd,
  NoCmd
};

WebCommand webCommand = NoCmd;              // Command given through the web interface

// Start program times (can be changed through the web page)
uint8_t MinuteStartCommand = 0;             // The date/time to give the Start command
uint8_t HourStartCommand = 0;
uint8_t DayStartCommand = 0;
uint8_t MonthStartCommand = 0;

// Declare functions with default arguments
void printDateTime(time_t date, bool serialOnly = false);
bool parseHeatCommand(const String& command);

// The setup function that initializes everything
void setup() {
  Serial.begin(115200);

  // Initialize pins
  pinMode(START_PROGRAM, OUTPUT);
  pinMode(HOLD_TEMP, OUTPUT);
  pinMode(WARMER, OUTPUT);
  pinMode(COOLER, OUTPUT);

  digitalWrite(START_PROGRAM, LOW);
  digitalWrite(HOLD_TEMP, LOW);
  digitalWrite(WARMER, LOW);
  digitalWrite(COOLER, LOW);

  // Read from flash
  preferences.begin("Chronotherm", false); 
  retryPeriod = preferences.getUChar("retryPeriod", retryPeriod);
  bool dataPresent = preferences.getBool("dataPresent", false);
  if (dataPresent) {
    Timezone = preferences.getUChar("Timezone", Timezone);
    UseDST = preferences.getBool("UseDST", UseDST);
  } else {
    printLine("Using default configuration");
  }

  // Initialize Wi-Fi. Force the ESP to reset Wi-Fi and initialize correctly.
  Serial.print("WiFi status = ");
  Serial.println(WiFi.getMode());
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  delay(1000);
  Serial.print("Wi-Fi status = ");
  Serial.println(WiFi.getMode());
  // End Wi-Fi initialization

  // Connect to Wi-Fi network with SSID and password. Reboot if it continuously fails.
  Serial.print("Connecting to ");
  Serial.println(ssid);

  // Initialize Wi-Fi
  WiFi.begin(ssid, password);

  time_t firstCheckAt = now();
  while ((WiFi.waitForConnectResult() != WL_CONNECTED) && (now() < firstCheckAt + connectPeriod)) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.waitForConnectResult() == WL_CONNECTED) {
    // Print local IP address and start web server
    Serial.println("Wi-Fi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Print the SSID of the network you're attached to
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    // Print the received signal strength
    long rssi = WiFi.RSSI();
    Serial.print("Signal strength (RSSI): ");
    Serial.print(rssi);
    Serial.println(" dBm");
  } else {
    WiFi.disconnect();
    Serial.println();
    Serial.println("No Wi-Fi connection established, rebooting");

    delay(retryPeriod * 1000);
    if (retryPeriod < 600) {
      retryPeriod = retryPeriod * 2;
      preferences.putUChar("retryPeriod", retryPeriod);
    }
    preferences.end();
    ESP.restart();
  }

  // Set up SMTP client
  smtp_client.setInsecure();

  // Set up IMAP client
  imap_client.setInsecure();

  // Load from Preferences
  loadHeatCommands();

  // For NTP
  Udp.begin(localPort);
  printLine("Started listening for UDP packets");
  clockUpdateStarted = now();

  // Start the webserver
  server.begin();
}

// For more information, see https://bit.ly/4h9JR7p
void imapStatusCallback(IMAPStatus status) {
  printDateTime(now(), true);
  ReadyMail.printf(" ReadyMail[imap][%d]%s\n", status.state, status.text.c_str());
}

// For more information, see https://bit.ly/430BPan
void imapCommandCallback(IMAPCommandResponse response) {
  if (response.isComplete) {
    printDateTime(now(), true);
    ReadyMail.printf(" ReadyMail[cmd][%d] %s\n", imap.status().state,
                      response.errorCode < 0 ? "error" : "success");
  } else {
    printDateTime(now(), true);
    ReadyMail.printf(" ReadyMail[cmd][%d] %s\n", imap.status().state, response.text.c_str());
  }
}

// For more information, see https://bit.ly/3GObULu
void imapDataCallback(IMAPCallbackData &data) {
  // Showing envelope data
  if (data.event() == imap_data_event_search || data.event() == imap_data_event_fetch_envelope)
  {
    // Show additional search info
    if (data.event() == imap_data_event_search) {
      printLine("");
      printDateTime(now(), true);
      ReadyMail.printf(" Showing Search result %d (%d) of %d from %d\n", data.messageIndex() + 1,
                        data.messageNum(), data.messageAvailable(), data.messageFound());
    }

    // Headers data
    for (size_t i = 0; i < data.headerCount(); i++) {
      printDateTime(now(), true);
      ReadyMail.printf(" %s: %s\n%s", data.getHeader(i).first.c_str(), data.getHeader(i).second.c_str(), i == data.headerCount() - 1 ? "\n" : "");

      if (data.getHeader(i).first.indexOf("Subject") > -1) {
        String subject = data.getHeader(i).second;
        subject.toUpperCase();
        if (subject.indexOf("HEAT") > -1) {
          PendingMailCommand pendingCommand;
          pendingCommand.msgNum = data.messageNum();
          pendingCommand.subject = subject;
          pendingMailCommands.push_back(pendingCommand);
          ReadyMail.printf("%s%s%s\n", "Added message ", String(data.messageNum()), " for command handling");
          break;
        }
      }
    }
  }
}

// For more information, see http://bit.ly/474niML
void smtpStatusCallback(SMTPStatus status) {
  if (status.progress.available) {
    printDateTime(now(), true);
    ReadyMail.printf(" ReadyMail[smtp][%d] Uploading file %s, %d %% completed\n", status.state,
                        status.progress.filename.c_str(), status.progress.value);
  } else {
    printDateTime(now(), true);
    ReadyMail.printf(" ReadyMail[smtp][%d]%s\n", status.state, status.text.c_str());
  }
}

void sendEmail(String subject, String body = "") {
  smtp.connect(smtpServer, 465, smtpStatusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(emailAccount, emailPassword, readymail_auth_password);

    SMTPMessage msg;
    msg.headers.add(rfc822_from, fromAddress);
    msg.headers.add(rfc822_to, toAddress);
    msg.headers.add(rfc822_subject, subject);
    if (!body.isEmpty()) {
      body = body + "\n\nCommand format: HEAT [ON/OFF/REMOVE] [Temperature/Command to remove] [DD-MM] [HH:MM]\n\nSent by your Chronotherm III";
      msg.text.body(body);
      body.replace("\n", "<br>");
      body = "<html><body><p>" + body + "</p></body></html>";
      msg.html.body(body);
    } else {
      msg.text.body("Command format: HEAT [ON/OFF/REMOVE] [Temperature/Command to remove] [DD-MM] [HH:MM]\n\nSent by your Chronotherm III");
      msg.html.body("<html><body><p>Command format: HEAT [ON/OFF/REMOVE] [Temperature/Command to remove] [DD-MM] [HH:MM]<br></p><p>Sent by your Chronotherm III</p></body></html>");
    }

    configTime(0, 0, "pool.ntp.org");
    while (time(nullptr) < 100000) delay(100);
    msg.timestamp = time(nullptr);
    smtp.send(msg);

    printDateTime(now(), true);
    printNoLine(" ");
    printLine(subject);
  }
  smtp.stop();
}

// Send an NTP request to the time server at the given address
int sendNtpPacket(const char* host) {
  int result = 0;

  // Set all bytes in the buffer to 0
  memset(packetBuffer, 0, NtpPacketSize);

  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;  // LI, Version, Mode

  // All needed NTP fields have been given values, now
  // send a packet requesting a timestamp
  result = Udp.beginPacket(host, 123);  // NTP requests are to port 123
  Udp.write(packetBuffer, NtpPacketSize);
  Udp.endPacket();

  return result;
}

time_t getNtpTime() {
  time_t result = 0;

  // Check if a reply is available
  if (Udp.parsePacket()) {
    printLine("Packet received");

    // We've received a packet, read the data from it
    Udp.read(packetBuffer, NtpPacketSize);  // Read the packet into the buffer

    // The timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, extract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);

    // Combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    printNoLine("Seconds since Jan 1 1900 = ");
    printLine(String(secsSince1900));

    // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
    const unsigned long seventyYears = 2208988800UL;

    // Subtract seventy years:
    unsigned long epoch = secsSince1900 - seventyYears;

    printNoLine("UTC time is: ");
    printDateTime(epoch);
    printLine("");

    result = epoch;
  }

  return result;
}

// Daylight Saving Time starts at 2 a.m. on the last Sunday in March. The clock is then set 1 hour
// ahead to 3 o'clock. So this Sunday lasts only 23 hours; an hour shorter than a normal day.
// Winter time starts at 3 a.m. on the last Sunday in the month of October. The time is
// then set back 1 hour to 2 a.m. In practice this means that this day lasts 25 hours.
// This function determines whether DST is active on the date specified by parameter 'time'.
// Note that it does not look at the time to make this determination, so it returns true for the
// last Sunday in March, regardless of the time, and false for the last Sunday in October.
// Note: dayOfWeek(SUN) = 1, dayOfWeek(MON) = 2, etc.
bool dstActive(time_t time) {
  bool result = false;
  int dayParam = day(time);
  int monthParam = month(time);
  int dayOfWeekParam = dayOfWeek(time);

  // Check if today is a DST day
  if ((monthParam == 3 && dayParam >= 25 && dayOfWeekParam <= dayParam - 24) ||
      (monthParam == 10 && (dayParam < 25 || (dayParam >= 25 && dayParam - dayOfWeekParam <= 23))) ||
      (monthParam > 3 && monthParam < 10)) {
    result = true;
  }

  return result;
}

// Calculate the next sunrise and sunset
void setSunriseSunset() {
  // Initialize sunMoon
  sm.init(Timezone, Latitude, Longitude);
  sunRise = sm.sunRise() + DST * 60;
  sunSet = sm.sunSet() + DST * 60;
  printNoLine("Today's sunrise and sunset: ");
  printDateTime(sunRise);
  printNoLine(", ");
  printDateTime(sunSet);
  printLine("");
}

// Two functions to handle debug messages
void printNoLine(String text) {
  if (debugText.length() > MAX_DEBUG_TEXT) {
    debugText = debugText.substring(debugText.length() - MAX_DEBUG_TEXT / 2);
  }
  debugText += text;
  Serial.print(text);
}

void printLine(String text) {
  if (debugText.length() > MAX_DEBUG_TEXT) {
    debugText = debugText.substring(debugText.length() - MAX_DEBUG_TEXT / 2);
  }
  debugText += text + "<br>\n";
  Serial.println(text);
}

void printDateTime(time_t date, bool serialOnly) {
  char buff[20];
  sprintf(buff, "%2d-%02d-%4d %02d:%02d:%02d",
    day(date), month(date), year(date), hour(date), minute(date), second(date));
  if (serialOnly) {
    Serial.print(buff);
  } else {
    printNoLine(buff);
  }
}

// Function to parse date and time from strings.
// If dateStr is empty, date defaults to today.
// If timeStr is empty, time defaults to 0:00.
bool parseDateTime(String dateStr, String timeStr, struct tm &start) {
  time_t now_time;
  time(&now_time);
  struct tm now = *localtime(&now_time);

  if (!dateStr.isEmpty()) {
    int index = dateStr.indexOf('-');
    if (index > 0 && dateStr.length() >= 3) {
      int day = dateStr.substring(0, index).toInt();
      int month = dateStr.substring(index + 1).toInt();
      start.tm_mday = day;
      start.tm_mon = month - 1;
    } else {
      return false;
    }
  } else {
    start.tm_mday = now.tm_mday;
    start.tm_mon = now.tm_mon;
  }

  if (!timeStr.isEmpty()) {
    int index = timeStr.indexOf(':');
    if (index > 0 && timeStr.length() >= 3) {
      int hour = timeStr.substring(0, index).toInt();
      int minute = timeStr.substring(index + 1).toInt();
      start.tm_hour = hour;
      start.tm_min = minute;
    } else {
      return false;
    }
  } else {
    start.tm_hour = 0;
    start.tm_min = 0;
  }

  start.tm_sec = 0;
  start.tm_year = now.tm_year;

  if (start.tm_hour < 0 || start.tm_hour > 23 || start.tm_min < 0 || start.tm_min > 59 ||
      start.tm_mday < 1 || start.tm_mday > 31 || start.tm_mon < 0 || start.tm_mon > 11) {
    return false;
  }

  return true;
}

String serializeHeatCommand(const HeatCommand& cmd) {
  return String(cmd.action) + "|" +
         String(cmd.temperature) + "|" +
         cmd.date + "|" +
         cmd.time;
}

String prettyPrintHeatCommand(const HeatCommand& cmd) {
  return String(cmd.action == 1 ? "ON" : "OFF") + "|" +
         (cmd.temperature == -1 ? "No temp" : String(cmd.temperature)) + "|" +
         (cmd.date.isEmpty() ? "No date" : cmd.date) + "|" +
         (cmd.time.isEmpty() ? "No time" : cmd.time);
}

HeatCommand deserializeHeatCommand(const String& serialized) {
  HeatCommand cmd;
  int pos1 = serialized.indexOf('|');
  int pos2 = serialized.indexOf('|', pos1 + 1);
  int pos3 = serialized.indexOf('|', pos2 + 1);

  cmd.action = serialized.substring(0, pos1).toInt();
  cmd.temperature = serialized.substring(pos1 + 1, pos2).toInt();
  cmd.date = serialized.substring(pos2 + 1, pos3);
  cmd.time = serialized.substring(pos3 + 1);

  return cmd;
}

void saveHeatCommands() {
  // Serialize all commands into a single string
  String serializedAll = "";
  for (int i = 0; i < commandCount; i++) {
    if (i > 0) serializedAll += ";"; // Separator between commands
    serializedAll += serializeHeatCommand(commands[i]);
  }

  // Store the serialized string
  preferences.putString("commands", serializedAll);
}

void loadHeatCommands() {
  String serializedAll = preferences.getString("commands", "");

  commandCount = 0;
  int start = 0;
  int end = serializedAll.indexOf(';');

  while (end != -1 && commandCount < MAX_COMMANDS) {
    String serializedCmd = serializedAll.substring(start, end);
    commands[commandCount] = deserializeHeatCommand(serializedCmd);
    commandCount++;
    start = end + 1;
    end = serializedAll.indexOf(';', start);
  }

  // Add the last command
  if (start < serializedAll.length() && commandCount < MAX_COMMANDS) {
    String serializedCmd = serializedAll.substring(start);
    commands[commandCount] = deserializeHeatCommand(serializedCmd);
    commandCount++;
  }
}

// Functions for controlling the Chronotherm

void startProgram(int temp = -1) {
    pushButton(START_PROGRAM);
    if (temp != -1) {
        setTemp(temp);
    }
}

void holdTemp(int temp = -1) {
    pushButton(HOLD_TEMP);
    if (temp != -1) {
        setTemp(temp);
    }
}

void setTemp(int temp) {
    // Lower to minimum first
    for (int i = 0; i < MAX_TEMP + 2 - MIN_TEMP; i++) {
        pushButton(COOLER);
    }
    // Limit temperature
    if (temp > MAX_TEMP) {
        temp = MAX_TEMP;
    }
    // Raise to desired temp
    for (int i = 0; i < temp - MIN_TEMP; i++) {
        pushButton(WARMER);
    }
}

void pushButton(int button) {
    digitalWrite(button, HIGH);
    delay(HOLD_TIME);
    digitalWrite(button, LOW);
    delay(IDLE_TIME);
}

void deleteHandledMessages() {
  // Delete handled messages
  if (!msgsToDelete.empty()) {
    printLine("Deleting messages");

    // Delete in reverse order to keep sequence numbers valid
    std::sort(msgsToDelete.begin(), msgsToDelete.end(), std::greater<uint32_t>());

    for (uint32_t msgNum : msgsToDelete) {
      printNoLine("Deleting message ");
      printLine(String(msgNum));
      String storeCmd = "STORE " + String(msgNum) + " +FLAGS (\\Deleted)";
      imap.sendCommand(storeCmd, imapCommandCallback, AWAIT_MODE);
    }
    imap.sendCommand("EXPUNGE", imapCommandCallback, AWAIT_MODE);
    msgsToDelete.clear();
  }
}

void processPendingMailCommands() {
  if (!pendingMailCommands.empty()) {
    for (PendingMailCommand pendingCommand : pendingMailCommands) {
      parseHeatCommand(pendingCommand.subject);
      ReadyMail.printf("%s%s%s\n", "Added message ", String(pendingCommand.msgNum), " for deletion");
      msgsToDelete.push_back(pendingCommand.msgNum);
    }
    pendingMailCommands.clear();
  }
}

// Parse a command string and add valid commands to the buffer
bool parseHeatCommand(const String& command) {
  HeatCommand newCommand = {1, -1, "", ""};

  if (commandCount >= MAX_COMMANDS - 1) {
    sendEmail("Command buffer full");
    return false;
  }

  // Convert to uppercase for case insensitivity
  String upperCommand = command;
  upperCommand.toUpperCase();
  upperCommand.trim();

  // Split into tokens (space-separated)
  int spaceIndex = upperCommand.indexOf(' ');
  String cmd = (spaceIndex == -1) ? upperCommand : upperCommand.substring(0, spaceIndex);

  // Check if the command starts with HEAT
  if (cmd != "HEAT") {
    sendEmail("Invalid command", command);
    return false; // Return if not a HEAT command
  }

  // Parse the rest of the command
  String rest = (spaceIndex == -1) ? "" : upperCommand.substring(spaceIndex + 1);
  rest.trim();

  // Parse ON/OFF (optional, default ON)
  spaceIndex = rest.indexOf(' ');
  String stateStr = (spaceIndex == -1) ? rest : rest.substring(0, spaceIndex);
  if (stateStr == "OFF") {
    newCommand.action = 0;
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  } else if (stateStr == "ON") {
    newCommand.action = 1;
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  } else if (stateStr == "STATUS") {
    newCommand.action = 2;
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  } else if (stateStr == "REMOVE") {
    newCommand.action = 3;
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  }
  rest.trim();

  // Parse temperature (if present)
  spaceIndex = rest.indexOf(' ');
  String tempStr = (spaceIndex == -1) ? rest : rest.substring(0, spaceIndex);
  if (tempStr.length() > 0 && tempStr.indexOf('-') == -1 && tempStr.indexOf(':') == -1) {
    newCommand.temperature = tempStr.toInt();
    if (newCommand.temperature == 0 && tempStr != "0") {
      printNoLine("Invalid temperature: ");
      printLine(tempStr);
      sendEmail("Invalid temperature in command (" + tempStr + ")");
      return false;
    }
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  }
  rest.trim();

  // Parse date (if present)
  spaceIndex = rest.indexOf(' ');
  String date = (spaceIndex == -1) ? rest : rest.substring(0, spaceIndex);
  if (date.length() > 0 && date.indexOf('-') > -1) {
    struct tm dummy;
    if (parseDateTime(date, "", dummy)) {
      newCommand.date = date;
    } else {
      printNoLine("Invalid date: ");
      printLine(date);
      sendEmail("Invalid date in command (" + date + ")");
      return false;
    }
    rest = (spaceIndex == -1) ? "" : rest.substring(spaceIndex + 1);
  }
  rest.trim();

  // Parse time (if present, default "0:00"), ignore everything else
  spaceIndex = rest.indexOf(' ');
  String time = (spaceIndex == -1) ? rest : rest.substring(0, spaceIndex);
  if (time.length() > 0 && time.indexOf(':') > -1) {
    struct tm dummy;
    if (parseDateTime("", time, dummy)) {
      newCommand.time = time;
    } else {
      printNoLine("Invalid time: ");
      printLine(time);
      sendEmail("Invalid time in command (" + time + ")");
      return false;
    }
  }

  if (newCommand.action == 0 || newCommand.action == 1) {
    // Add command to the buffer
    commands[commandCount] = newCommand;
    commandCount++;

    // Save to preferences
    saveHeatCommands();
  } else if (newCommand.action == 3 && newCommand.temperature > 0) {
    // Remove command from buffer
    for (size_t i = newCommand.temperature - 1; i < commandCount - 1; i++) {
      commands[i] = commands[i + 1];
    }
    commandCount--;

    // Save to preferences
    saveHeatCommands();
  }

  // Notify user
  String body = "Commands in buffer:\n";
  for (int j = 0; j < commandCount; j++) {
    body += prettyPrintHeatCommand(commands[j]) + "\n";
  }
  sendEmail("Handled command " + upperCommand, body);

  printNoLine("Command: ");
  printLine(serializeHeatCommand(newCommand));

  return true;
}

// Function to handle commands from email subject
void handleHeatCommands() {
  struct tm start;

  // Check if any of the heat command needs to be executed
  for (size_t i = 0; i < commandCount; i++) {
    // Check if the date / time has arrived
    parseDateTime(commands[i].date, commands[i].time, start);

    time_t now_time;
    time(&now_time);
    time_t start_time = mktime(&start) - (Timezone + DST) * 60;

    // Skip commands that must be executed sometime in the future
    if (start_time > now_time) {
      continue;
    }

    // Handle the command
    if (commands[i].action == 1) {
      if (commands[i].temperature > -1) {
        startProgram(commands[i].temperature);
        sendEmail("Program started, temporary temperature: " + String(commands[i].temperature) + "°C");
      } else {
        startProgram();
        sendEmail("Program started");
      }
    } else {
      if (commands[i].temperature > -1) {
        holdTemp(commands[i].temperature);
        sendEmail("Constant temperature: " + String(commands[i].temperature) + "°C");
      } else {
        holdTemp();
        sendEmail("Constant temperature set");
      }
    }

    // Remove handled commands from the buffer
    for (size_t j = i; j < commandCount - 1; j++) {
      commands[j] = commands[j + 1];
    }
    commandCount--;

    // Save to preferences
    saveHeatCommands();
  }
}

void handleWebClient() {
  char buf[20];
  String text;

  if (!clientActive) {
    client = server.available();            // Listen for incoming clients
    if (client) {                           // If a new client connects
      clientConnectedAt = now();
      printNoLine("New client connection at ");
      printDateTime(clientConnectedAt);
      printLine("");
      clientActive = true;
      currentLine = "";
    }
  } else {
    if (client.connected()) {               // Check if the client is still connected
      if (client.available()) {             // If there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;
        if (c == '\n') {                    // If the byte is a newline character
          // If the current line is blank, you got two newline characters in a row.
          // That's the end of the client HTTP request, so send a response
          if (currentLine.length() == 0) {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            // Handle user input
            if (header.indexOf("GET /up1deg") >= 0) {
              printLine("1 degree up selected");
              webCommand = UpCmd;
            } else if (header.indexOf("GET /down1deg") >= 0) {
              printLine("1 degree down selected");
              webCommand = DownCmd;
            } else if (header.indexOf("GET /hold") >= 0) {
              printLine("Hold temp selected");
              webCommand = HoldCmd;
            } else if (header.indexOf("GET /start") >= 0) {
              printLine("Start program selected");
              webCommand = StartCmd;
            } else if (header.indexOf("GET /params") >= 0) {
              // Find first parameter
              int begin = header.indexOf('?');
              int end = header.indexOf('&');
              if (begin > -1 && end > begin + 1) {
                String sub = header.substring(begin + 1, end);

                // Split parameter in name and value. Repeat for all parameters
                bool dataPresent = true;
                bool invalidParam = false;

                UseDST = false;

                do {
                  int equalsPos = sub.indexOf('=');
                  if (equalsPos > -1) {
                    String param = sub.substring(0, equalsPos);
                    String value = sub.substring(equalsPos + 1, sub.length());

                    // Check for and set the variables
                    long temp;
                    if (param.indexOf("Revert") == 0) {
                      dataPresent = false;
                    } else if (param.indexOf("Timezone") == 0) {
                      temp = value.toInt();
                      if (temp >= 0 && temp <= 23) Timezone = temp;
                    } else if (param.indexOf("UseDST") == 0) {
                      UseDST = true;
                    } else if (param.indexOf("MonthStartCommand") == 0) {
                      temp = value.toInt();
                      if (temp >= 0 && temp <= 12) MonthStartCommand = temp;
                    } else if (param.indexOf("DayStartCommand") == 0) {
                      temp = value.toInt();
                      if (temp >= 0 && temp <= 31) DayStartCommand = temp;
                    } else if (param.indexOf("HourStartCommand") == 0) {
                      temp = value.toInt();
                      if (temp >= 0 && temp <= 23) HourStartCommand = temp;
                    } else if (param.indexOf("MinuteStartCommand") == 0) {
                      temp = value.toInt();
                      if (temp >= 0 && temp <= 59) MinuteStartCommand = temp;
                    } else {
                      invalidParam = true;
                    }
                  } else {
                    invalidParam = true;
                  }

                  // Go to next parameter
                  begin = end + 1;
                  end = header.indexOf('&', begin);
                  if (end < 0 || end > header.indexOf(' ', begin)) {
                    end = header.indexOf(' ', begin);
                    if (end < 0) {
                      end = header.length();
                    }
                  }

                  if (end > begin + 1) {
                    sub = header.substring(begin, end);
                  }
                } while (!invalidParam && end > begin + 1);

                // Use putBool for dataPresent and UseDST to match getBool on read
                preferences.putBool("dataPresent", dataPresent);
                preferences.putUChar("Timezone", Timezone);
                preferences.putBool("UseDST", UseDST);

                // Add new command to the queue
                if (MonthStartCommand != 0 && DayStartCommand != 0) {
                  String command = "HEAT ON " + String(DayStartCommand) + "-" + String(MonthStartCommand) + " " + String(HourStartCommand) + ":" + String(MinuteStartCommand);
                  parseHeatCommand(command);
                }
              }
            }

            time_t t_now = now();

            // Display the HTML web page
            client.println("<!DOCTYPE html><html>");
            client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("<link rel=\"icon\" href=\"data:,\">");

            // CSS to style the on/off buttons
            // Feel free to change the background-color and font-size attributes to fit your preferences
            client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
            client.println(".button { background-color: #195B6A; border: none; color: white; padding: 16px 40px;");
            client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
            client.println(".button2 {background-color: #77878A;}</style></head>");

            // Web Page Heading
            client.println("<body><h1>Chronotherm III</h1>");
            client.print("<p>Today date/time: ");
            sprintf(buf, "%2d-%02d-%4d %02d:%02d:%02d",
              day(t_now), month(t_now), year(t_now), hour(t_now), minute(t_now), second(t_now));
            client.print(buf);
            client.println("</p>");

            client.print("<p>Today's sunrise and sunset: ");
            sprintf(buf, "%2d-%02d-%4d %02d:%02d:%02d",
              day(sunRise), month(sunRise), year(sunRise), hour(sunRise), minute(sunRise), second(sunRise));
            client.print(buf);
            client.print(", ");
            sprintf(buf, "%2d-%02d-%4d %02d:%02d:%02d",
              day(sunSet), month(sunSet), year(sunSet), hour(sunSet), minute(sunSet), second(sunSet));
            client.print(buf);
            client.println("</p>");

            // Print the current command queue
            if (commandCount > 0) {
              client.print("<p>Command queue:</p>");
              for (int i = 0; i < commandCount; i++) {
                text = "<p>" + String(i + 1) + ":" + prettyPrintHeatCommand(commands[i]) + "</p>";
                client.print(text);
              }
            } else {
              client.print("<p>No commands in the command queue</p>");
            }

            client.println("<p><a href=\"/up1deg\"><button class=\"button\">Up 1 degree</button></a></p>");
            client.println("<p><a href=\"/down1deg\"><button class=\"button\">Down 1 degree</button></a></p>");
            client.println("<p><a href=\"/hold\"><button class=\"button\">Hold temperature</button></a></p>");
            client.println("<p><a href=\"/start\"><button class=\"button\">Start program</button></a></p>");
            client.println("<p><a href=\"/\"><button class=\"button\">Refresh</button></a></p><br>");

            // Print the form for uploading settings
            client.println("<form action=\"/params\">");
            client.println("<p>Revert to default values after reset:<input type=\"checkbox\" name=\"Revert\"></p>");
            client.print("<p>Timezone:<input type=\"text\" name=\"Timezone\" value=\"");
            client.print(Timezone);
            client.println("\"></p>");
            client.print("<p>Observe DST:<input type=\"checkbox\" name=\"UseDST\"");
            if (UseDST) client.print(" checked");
            client.println("></p>");
            client.print("<p>Give Start program command:</p>");
            client.print("<p>Month:<input type=\"text\" name=\"MonthStartCommand\" value=\"");
            client.print(MonthStartCommand);
            client.println("\"></p>");
            client.print("<p>Day:<input type=\"text\" name=\"DayStartCommand\" value=\"");
            client.print(DayStartCommand);
            client.println("\"></p>");
            client.print("<p>Hour:<input type=\"text\" name=\"HourStartCommand\" value=\"");
            client.print(HourStartCommand);
            client.println("\"></p>");
            client.print("<p>Minute:<input type=\"text\" name=\"MinuteStartCommand\" value=\"");
            client.print(MinuteStartCommand);
            client.println("\"></p>");
            client.println("<p><input type=\"submit\" value=\"Submit\"></p>");
            client.println("</form>");

            client.print("<p>");
            client.print(debugText);
            client.println("</p>");

            client.println("</body></html>");

            // The HTTP response ends with another blank line
            client.println();

            // Clear the header variable
            header = "";

            // Close the connection
            client.stop();
            printLine("Client disconnected");
            printLine("");

            clientActive = false;
          } else {  // If you got a newline, then clear currentLine
            currentLine = "";
          }
        } else if (c != '\r') {  // If you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      } else {
        // The client is connected but no data is available. Time out if this takes too long.
        if (now() > clientConnectedAt + webConnectPeriod) {
          clientActive = false;
          printLine("Client connection timed out");
        }
      }
    } else {
      clientActive = false;
      printLine("Client no longer connected");
    }
  }
}

void handleWebCommand() {
  switch (webCommand) {
  case DownCmd:
    pushButton(COOLER);
    break;
  case UpCmd:
    pushButton(WARMER);
    break;
  case HoldCmd:
    pushButton(HOLD_TEMP);
    break;
  case StartCmd:
    pushButton(START_PROGRAM);
    break;
  }
  webCommand = NoCmd;
}

void loop() {
  // Set the time when not set
  if (!ntpTimeSet) {
    // If not set, check for the time, but only for a few minutes.
    // In order not to do these checks every loop, proceed only once a second
    if ((now() < clockUpdateStarted + UpdateTimeTimeout) && (lastSecond != second())) {
      // Send a packet every 30 seconds and check for an answer the other times
      if (second() % 30 == 8) {
        printNoLine("Requesting time from NTP server at ");
        printDateTime(now());
        printLine("");

        // Send an NTP packet to a time server
        if (sendNtpPacket(NtpServer) == 0) {
          printNoLine("DNS lookup failed for ");
          printLine(NtpServer);
        }
      } else {
        // Check for received packets
        time_t epoch = getNtpTime();
        if (epoch > 0) {
          printNoLine("Daylight Saving Time ");
          if (UseDST) {
            printNoLine("observed ");
            if (dstActive(epoch)) {
              DST = 60;
              printLine("and active");
            } else {
              DST = 0;
              printLine("but not active");
            }
          } else {
            printLine("not observed");
          }

          printNoLine("Setting system time to UTC + ");
          printNoLine(String(Timezone + DST));
          printLine(" minutes");
          setTime(epoch + (Timezone + DST) * 60);
          ntpTimeInitialized = true;
          ntpTimeSet = true;

          // Catch emails that arrived while the device was offline on the next IMAP poll.
          imapUpdateStarted = 0;

          setSunriseSunset();
        }
      }
      lastSecond = second();
    } else if (now() > clockUpdateStarted + 3600) {
      // After an hour, try it again.
      clockUpdateStarted = now();
    } else if (now() > clockUpdateStarted + UpdateTimeTimeout) {
      // No time packet was received, send a warning email once.
      // Chances are the internet connection is down anyway.
      if (!emailSent) {
        emailSent = true;
        sendEmail("Problem connecting to NTP server");
      }
    }
  } else {
    // Make sure the clock is updated once a week on Sunday mornings.
    // This also allows for DST to be set correctly. Note that time can move
    // back or forward by one hour.
    if (dayOfWeek(now()) == 1 && hour() == 1 && minute() == 10 && second() == 0) {
      printNoLine("Invalidating time at ");
      printDateTime(now());
      printLine("");
      ntpTimeSet = false;
      clockUpdateStarted = now();
      emailSent = false;
    }
  }

  time_t t_now = now();  // The number of seconds since Jan 1 1970

  // Every day at two thirty a.m. calculate the next sunrise and sunset
  if (!settingSunRiseSunSet && hour() == 2 && minute() == 30 && second() == 0) {
    settingSunRiseSunSet = true;
    printNoLine("Calculating sunrise and sunset at ");
    printDateTime(t_now);
    printLine("");

    setSunriseSunset();
  } else if (settingSunRiseSunSet && second() != 0) {
    settingSunRiseSunSet = false;
  }

  // Handle user input
  handleWebClient();
  handleWebCommand();
  handleHeatCommands();

  // Catch lost Wi-Fi connections
  if (!WiFi.isConnected()) {
    imap.logout();
    WiFi.disconnect();
    Serial.println("No wifi connection, rebooting");
    delay(retryPeriod * 1000);
    if (retryPeriod < 600) {
      retryPeriod = retryPeriod * 2;
      preferences.putUChar("retryPeriod", retryPeriod);
    }
    preferences.end();
    ESP.restart();
  } else {
    if (retryPeriod > RETRY_PERIOD) {
      retryPeriod = RETRY_PERIOD;
      preferences.putUChar("retryPeriod", retryPeriod);
    }
  }

  if (ntpTimeInitialized && (now() > imapUpdateStarted + ReadMailFrequency || imapUpdateStarted == 0)) {
    imapUpdateStarted = now();
    pendingMailCommands.clear();

    // Gmail can close an idle TLS session between polls while ReadyMail still
    // reports it as connected. Use a fresh IMAP session for each mail check.
    imap.stop();
    delay(250);

    printLine("Opening fresh IMAP session");
    imap.connect(imapServer, 993, imapStatusCallback);
    if (imap.isConnected()) {
      imap.authenticate(emailAccount, emailPassword, readymail_auth_password, AWAIT_MODE);
    }

    if (imap.isAuthenticated()) {
      if (imap.select("INBOX", false)) {
        imap.search("SEARCH SUBJECT \"HEAT\"", 20, true, imapDataCallback, AWAIT_MODE);
        processPendingMailCommands();
        deleteHandledMessages();
      } else {
        printLine("IMAP select failed");
      }
    } else {
      printLine("IMAP authentication failed");
    }

    imap.logout();
    imap.stop();
  }
}
