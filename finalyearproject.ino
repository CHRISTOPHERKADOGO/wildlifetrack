#include <TinyGPS++.h>
#include <SoftwareSerial.h>

// Pin Definitions
const int SIM800L_RX = 3;
const int SIM800L_TX = 4;
const int GPS_RX = 7;
const int GPS_TX = 6;

// Communication Baud Rates
const int BAUD_RATE = 9600;

// SIM800L Settings
const String TOKEN = "74d87bed9142bf5ab5ef324bd09b8c920872d89896340b21606a10f226c2c5da";
const String ANIMAL_NUMBER = "KidepoNP-18-2";
const String APN = "airtel"; // Replace with your carrier's APN

// TinyGPS++ and SoftwareSerial Objects
TinyGPSPlus gps;
SoftwareSerial sim800l(SIM800L_RX, SIM800L_TX);
SoftwareSerial gpsSerial(GPS_RX, GPS_TX);

// Tracking variables
unsigned long lastSendTime = 0;
const unsigned long SEND_INTERVAL = 300000; // 5 minutes in milliseconds
const unsigned long RESTART_DELAY = 180000; // 3 minutes delay before restarting
unsigned long restartTime = 0; // Time when restart was triggered
bool gpsInitialized = false;
bool sim800lInitialized = false;
bool isGPSValid = false;
bool restartProcess = false;
bool waitingForRestart = false;

void setup() {
  // Start with the serial monitor for debug output
  Serial.begin(BAUD_RATE);
  Serial.println(F("Wildlife Tracker Initializing..."));
  delay(1000); // Give serial monitor time to start

  // Initialize GPS module only
  initializeGPS();
}

void loop() {
  // First check if we're in the waiting period before restart
  if (waitingForRestart) {
    if (millis() - restartTime >= RESTART_DELAY) {
      Serial.println(F("3-minute delay completed. Now restarting the process..."));
      waitingForRestart = false;
      resetSystem();
    } else {
      // Still waiting, show countdown
      unsigned long remainingTime = (restartTime + RESTART_DELAY - millis()) / 1000;
      static unsigned long lastCountdownUpdate = 0;
      
      // Update countdown every 15 seconds
      if (millis() - lastCountdownUpdate >= 15000) {
        lastCountdownUpdate = millis();
        Serial.print(F("Waiting before restart: "));
        Serial.print(remainingTime);
        Serial.println(F(" seconds remaining"));
      }
      return; // Skip the rest of the loop while waiting
    }
  }
  
  // Check if we need to restart the process
  if (restartProcess) {
    Serial.println(F("Data sent successfully. Waiting for 3 minutes before restarting..."));
    restartTime = millis();
    waitingForRestart = true;
    restartProcess = false;
    return;
  }
  
  // First phase: Wait for valid GPS coordinates
  if (!isGPSValid) {
    waitForValidGPS();
  } 
  // Second phase: Initialize SIM800L if GPS is valid and SIM800L not yet initialized
  else if (isGPSValid && !sim800lInitialized) {
    initializeSIM800L();
  }
  // Third phase: Normal operation - sending data periodically
  else if (isGPSValid && sim800lInitialized) {
    // Process any available GPS data
    while (gpsSerial.available() > 0) {
      char c = gpsSerial.read();
      if (gps.encode(c)) {
        if (gps.location.isValid()) {
          displayLocation();
        }
      }
    }
    
    // Check if it's time to send data
    unsigned long currentTime = millis();
    if (currentTime - lastSendTime >= SEND_INTERVAL) {
      Serial.println(F("Time to send update..."));
      
      if (gps.location.isValid()) {
        Serial.println(F("GPS location is valid, sending to server"));
        if (configureGPRS()) {
          sendLocationToServer();
          // Set flag to restart the process after successful data send
          restartProcess = true;
        } else {
          Serial.println(F("Failed to configure GPRS"));
        }
      } else {
        Serial.println(F("GPS location not valid, skipping update"));
      }
      
      lastSendTime = currentTime;
    }
  }
}

// Reset the system to start the process again
void resetSystem() {
  Serial.println(F("Restarting the entire process..."));
  
  // Close any open connections
  if (sim800lInitialized) {
    sim800l.println(F("AT+HTTPTERM"));
    delay(1000);
    readResponse();
    
    sim800l.println(F("AT+SAPBR=0,1")); // Close bearer if open
    delay(2000);
    readResponse();
  }
  
  // Reset variables
  gpsInitialized = false;
  sim800lInitialized = false;
  isGPSValid = false;
  restartProcess = false;
  
  // Reinitialize GPS module
  initializeGPS();
}

// Initialize GPS module
void initializeGPS() {
  Serial.println(F("Initializing GPS module..."));
  
  // If GPS was previously initialized, end the software serial
  if (gpsInitialized) {
    gpsSerial.end();
    delay(1000);
  }
  
  gpsSerial.begin(BAUD_RATE);
  gpsInitialized = true;
  Serial.println(F("GPS module initialized. Waiting for valid GPS coordinates..."));
}

// Wait for valid GPS coordinates
void waitForValidGPS() {
  static unsigned long gpsStartTime = millis();
  static unsigned long lastStatusTime = 0;
  
  // Process any GPS data
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
  }
  
  // Check if GPS has valid coordinates
  if (gps.location.isValid()) {
    isGPSValid = true;
    Serial.println(F("Valid GPS coordinates obtained!"));
    displayLocation();
    return;
  }
  
  // Show status every 10 seconds while waiting
  unsigned long currentTime = millis();
  if (currentTime - lastStatusTime >= 10000) {
    lastStatusTime = currentTime;
    
    if (gps.charsProcessed() < 10 && (currentTime - gpsStartTime > 10000)) {
      Serial.println(F("WARNING: No GPS data received. Check GPS wiring."));
    } else {
      Serial.println(F("Waiting for valid GPS fix..."));
      Serial.print(F("Chars processed: "));
      Serial.println(gps.charsProcessed());
      Serial.print(F("Satellites visible: "));
      if (gps.satellites.isValid()) {
        Serial.println(gps.satellites.value());
      } else {
        Serial.println(F("Unknown"));
      }
    }
  }
}

// Initialize SIM800L module
void initializeSIM800L() {
  Serial.println(F("GPS coordinates valid. Now initializing GSM module..."));
  
  // If SIM800L was previously initialized, end the software serial
  if (sim800lInitialized) {
    sim800l.end();
    delay(1000);
  }
  
  sim800l.begin(BAUD_RATE);
  delay(3000); // Give SIM800L time to power up
  
  // Test AT command multiple times
  bool gsmResponding = false;
  for (int i = 0; i < 3; i++) {
    Serial.print(F("Testing GSM communication (attempt "));
    Serial.print(i+1);
    Serial.println(F(")"));
    
    sim800l.flush(); // Clear any pending data
    sim800l.println(F("AT"));
    delay(1000);
    
    String response = "";
    unsigned long startTime = millis();
    while ((millis() - startTime) < 3000) {
      if (sim800l.available()) {
        char c = sim800l.read();
        response += c;
      }
    }
    
    Serial.print(F("Response: "));
    Serial.println(response);
    if (response.indexOf("OK") >= 0) {
      Serial.println(F("GSM module is responding!"));
      gsmResponding = true;
      break;
    }
    
    delay(1000);
  }
  
  if (!gsmResponding) {
    Serial.println(F("WARNING: GSM module not responding after 3 attempts. Check wiring and power."));
    sim800lInitialized = false;
    return;
  }
  
  // Additional setup for GSM module if it's responding
  sim800l.println(F("AT+CMGF=1")); // Set to text mode
  delay(1000);
  readResponse();
  
  sim800lInitialized = true;
  lastSendTime = 0; // Reset timer to trigger an immediate send
  Serial.println(F("SIM800L module initialized successfully!"));
}

// Configure GPRS connection
bool configureGPRS() {
  Serial.println(F("Configuring GPRS connection..."));
  
  // First try to get status and close any existing connection
  sim800l.println(F("AT+SAPBR=2,1"));
  delay(2000);
  readResponse();
  
  sim800l.println(F("AT+SAPBR=0,1")); // Close bearer if open
  delay(2000);
  readResponse();
  
  // Configure connection parameters
  sim800l.println(F("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\""));
  delay(2000);
  if (!readResponse()) return false;
  
  // Set APN
  char apnCommand[50]; 
  snprintf(apnCommand, sizeof(apnCommand), "AT+SAPBR=3,1,\"APN\",\"%s\"", APN.c_str());
  sim800l.println(apnCommand);
  delay(2000);
  if (!readResponse()) return false;
  
  // Open bearer
  Serial.println(F("Opening GPRS bearer..."));
  sim800l.println(F("AT+SAPBR=1,1"));
  delay(10000); // Give more time for network registration
  if (!readResponse()) {
    Serial.println(F("First attempt failed, trying again..."));
    delay(2000);
    sim800l.println(F("AT+SAPBR=1,1"));
    delay(10000);
    if (!readResponse()) return false;
  }
  
  // Check bearer status
  sim800l.println(F("AT+SAPBR=2,1"));
  delay(2000);
  String response = readResponseString();
  
  // Parse the response to verify connection
  if (response.indexOf("+SAPBR: 1,1") >= 0) {
    Serial.println(F("GPRS connection established successfully"));
    return true;
  } else {
    Serial.println(F("Failed to establish GPRS connection"));
    return false;
  }
}

// Send location data to server
void sendLocationToServer() {
  if (!gps.location.isValid()) {
    Serial.println(F("Cannot send location - GPS data not valid"));
    return;
  }
  
  Serial.println(F("Preparing to send data to server..."));
  
  String latitude = String(gps.location.lat(), 6);
  String longitude = String(gps.location.lng(), 6);

  String url = "http://35.185.63.144:8000/proxy?token=" + TOKEN + 
               "&animal_number=" + ANIMAL_NUMBER + 
               "&latitude=" + latitude + 
               "&longitude=" + longitude;

  Serial.print(F("URL: "));
  Serial.println(url);
  
  // Terminate any existing HTTP service first
  sim800l.println(F("AT+HTTPTERM"));
  delay(1000);
  readResponse();
  
  // Initialize HTTP service
  sim800l.println(F("AT+HTTPINIT"));
  delay(2000);
  if (!readResponse()) {
    Serial.println(F("HTTP initialization failed"));
    return;
  }
  
  // Set parameters
  sim800l.println(F("AT+HTTPPARA=\"CID\",1"));
  delay(1000);
  if (!readResponse()) return;
  
  // Method 1: Using a buffer for URL parameter
  char urlCommand[200]; // Increased buffer size for longer URLs
  snprintf(urlCommand, sizeof(urlCommand), "AT+HTTPPARA=\"URL\",\"%s\"", url.c_str());
  sim800l.println(urlCommand);
  
  delay(1000);
  if (!readResponse()) return;
  
  // Start HTTP GET session
  Serial.println(F("Sending data to server..."));
  sim800l.println(F("AT+HTTPACTION=0"));
  delay(15000); // Longer timeout for network response
  
  String actionResponse = readResponseString();
  if (actionResponse.indexOf("+HTTPACTION: 0,200") >= 0) {
    sim800l.println(F("AT+HTTPREAD"));
    delay(5000);
    readResponse();
    Serial.println(F("Data sent successfully!"));
  } else {
    Serial.print(F("Failed to send data. Response: "));
    Serial.println(actionResponse);
  }
  
  // Terminate HTTP service
  sim800l.println(F("AT+HTTPTERM"));
  delay(1000);
  readResponse();
}

// Display GPS location data
void displayLocation() {
  Serial.println(F("------ GPS DATA ------"));
  Serial.print(F("Latitude: "));
  Serial.println(gps.location.lat(), 6);
  Serial.print(F("Longitude: "));
  Serial.println(gps.location.lng(), 6);
  
  if (gps.altitude.isValid()) {
    Serial.print(F("Altitude: "));
    Serial.print(gps.altitude.meters());
    Serial.println(F(" meters"));
  }
  
  if (gps.speed.isValid()) {
    Serial.print(F("Speed: "));
    Serial.print(gps.speed.kmph());
    Serial.println(F(" km/h"));
  }
  
  if (gps.satellites.isValid()) {
    Serial.print(F("Satellites: "));
    Serial.println(gps.satellites.value());
  }
  
  Serial.println(F("---------------------"));
}

// Helper function to read and print response
bool readResponse() {
  String response = readResponseString();
  return (response.indexOf("ERROR") < 0);
}

// Helper function to read response as string
String readResponseString() {
  String response = "";
  unsigned long startTime = millis();
  
  while ((millis() - startTime) < 3000) {
    while (sim800l.available()) {
      char c = sim800l.read();
      response += c;
    }
  }
  
  Serial.print(F("Response: "));
  Serial.println(response);
  return response;
}