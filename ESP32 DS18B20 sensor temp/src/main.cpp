#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <sys/time.h> // For RTC adjustment
#include <Preferences.h>
#include <esp_sleep.h>
#include "driver/rtc_io.h"
#include "soc/rtc.h"

Preferences preferences;

// AP credentials
const char* ssid = "ESP32_Temperature_AP";
const char* password = "12345678";

// Timing variables for periodic temperature recording
unsigned long lastRecordTime = 0;
const unsigned long recordInterval = 10 * 1000; // 10 seconds (when awake)
int recordIntervalMinutes = 2; // Default interval is 2 minutes (during sleep)
int MeasurementCounter = 0;
// Store a bool in RTC Slow Memory 
RTC_DATA_ATTR bool firstExecution = true;  

#define WAKEUP_PIN GPIO_NUM_10 // Replace with your GPIO pin number

String sensorID;

// Web server on port 80
WebServer server(80);

// GPIO where the DS18B20 is connected
#define ONE_WIRE_BUS 4

// Initialize OneWire and DallasTemperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);



// Function prototypes
void handleRoot();
void handleSetTime();
void handleFileDownload();
void handleDelete();
void handleSetGPS();
void handleSetInterval();
void recordTemperature();
void recordTemperatureAndSleep();
void saveDataToFile(const char* filename, const String& data);
String readDataFile();
void setSystemTimeFromPhone(String phoneTime, int timezoneOffset);

void setupRTCWithCrystal() {
    // Enable the 32.768 kHz external crystal
    rtc_clk_32k_enable(true);

    // Wait for the crystal to stabilize
    delay(200); // 200ms delay for stabilization

    // Check if the crystal is ready
    if (rtc_clk_slow_freq_get() == RTC_SLOW_FREQ_32K_XTAL) {
        Serial.println("32.768 kHz crystal is now the RTC clock source.");
    } else {
        Serial.println("Failed to set 32.768 kHz crystal as RTC clock source.");
    }
}

void setup() {
  // Start serial communication for debugging
  Serial.begin(115200);

  // Initialize SPIFFS (this is separate from Preferences)
  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS");
    return;
  }

  // Configure the GPIO pin as an input with pull-down
  pinMode(WAKEUP_PIN, INPUT_PULLDOWN);

  //The ESP32 has two clock sources for the RTC. External 32.768 kHz Crystal: More precise and recommended for long-term accuracy
  setupRTCWithCrystal();

  // Set the GPIO as a wakeup source on high level
  //esp_sleep_enable_ext1_wakeup(1ULL << WAKEUP_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
  gpio_wakeup_enable(WAKEUP_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1 or wakeupReason == ESP_SLEEP_WAKEUP_GPIO) {
    firstExecution = true;
    Serial.println("Wakeup caused by GPIO pin high level");
  }

  // Initialize temperature sensor (make sure 'sensors' object is defined)
  // Example: DallasTemperature sensors(oneWire);
  sensors.begin();


  if (firstExecution) {
    Serial.println("It's the first execution.");
    firstExecution = false;  // Set to false after first execution

  // Configure ESP32 as an Access Point
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.println("Access Point started");
  Serial.println("Connect to the network and go to: http://" + IP.toString());


  if (sensors.getDeviceCount() > 0) {
    DeviceAddress address;
    if (sensors.getAddress(address, 0)) {
      // Convert the sensor address to a human-readable ID
      sensorID = "";
      for (uint8_t i = 0; i < 8; i++) {
        sensorID += String(address[i], HEX);
        if (i < 7) sensorID += ":";
      }
      Serial.println("Sensor ID: " + sensorID);
    } else {
      sensorID = "Unknown_Sensor";
    }
  } else {
    sensorID = "No_Sensor_Found";
  }

  // Set up web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download", HTTP_GET, handleFileDownload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/set-gps", HTTP_POST, handleSetGPS);
  server.on("/set-interval", HTTP_POST, handleSetInterval);
  server.on("/start-measurements", HTTP_GET, recordTemperatureAndSleep);

  // Start the server
  server.begin();
  Serial.println("Web server started");

  // Record the first temperature value
  recordTemperature();
  }
  else
  {
    Serial.println("get temp before sleep");
    recordTemperatureAndSleep();
  }
}

void loop() {
  server.handleClient();

  // Check if it's time to record the temperature
  unsigned long currentTime = millis();
  if (currentTime - lastRecordTime >= recordInterval) {
    recordTemperature();
    lastRecordTime = currentTime;
  }
}

// Save data to SPIFFS file
void saveDataToFile(const char* filename, const String& data) {
  File file = SPIFFS.open(filename, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.print(data);
  file.close();
  Serial.println("Data saved to file: " + String(filename));
}

// Read the data file and return its contents as a String
String readDataFile() {
  const char* filename = "/data.txt";
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return "No data available.";
  }

  String data = "";
  while (file.available()) {
    data += (char)file.read();
  }
  file.close();
  return data;
}

String getLastMeasurement() {
    preferences.begin("lastData", true); // Read-only mode
    String measurement = preferences.getString("lastMeasure", "No data recorded yet.");
    preferences.end();
    return measurement;
}

void saveLastMeasurement(const String& measurement) {
    preferences.begin("lastData", false); // Namespace "lastData"
    preferences.putString("lastMeasure", measurement);
    preferences.end();
}

void saveCounterToFlash(int value) {
    preferences.begin("measurements", false);
    preferences.putInt("counter", value);
    preferences.end();
}

int getMeasurementCounter() {
    preferences.begin("measurements", true);  // Open preferences in read-only mode
    int counter = preferences.getInt("counter", 0);  // Default to 0 if no value is set
    preferences.end();
    return counter;
}

void incrementCounter() {
    // Start accessing the Preferences in read/write mode
    preferences.begin("measurements", false);
    
    // Load the current counter value, default to 0 if it doesn't exist
    int counter = preferences.getInt("counter", 0);
    
    // Increment the counter
    counter++;
    
    // Save the updated counter value back to flash
    preferences.putInt("counter", counter);
    
    // Close the Preferences to free resources
    preferences.end();
    
    // Optional: Update the in-memory variable if you're keeping one
    MeasurementCounter = counter;
}

// Record temperature and save to file
void recordTemperature() {
/*   static unsigned long lastRecordTime = 0;
  unsigned long currentMillis = millis();

  // Convert interval from minutes to milliseconds
  unsigned long intervalMillis = recordIntervalMinutes * 1000;  //add *60

  if (currentMillis - lastRecordTime >= intervalMillis) {
    lastRecordTime = currentMillis; */

    sensors.requestTemperatures();
    float temperature = sensors.getTempCByIndex(0);
    if (temperature == DEVICE_DISCONNECTED_C) {
      Serial.println("Error: Could not read temperature data");
      return;
    }

    // Get current time
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm* timeinfo = localtime(&now);

    // Prepare data for storage
    char timeStr[15];
    strftime(timeStr, sizeof(timeStr), "%Y%m%d %H:%M", timeinfo);
    String data = String(timeStr) + ";" + String(temperature) + "\n";

    saveLastMeasurement(data);
    incrementCounter();

    // Append to the file
    saveDataToFile("/data.txt", data);

    Serial.println("Temperature recorded: " + String(temperature) + "°C");
    Serial.print("Free Flash Size: ");Serial.println(SPIFFS.totalBytes() - SPIFFS.usedBytes());
  //}
}

void recordTemperatureAndSleep() {
  Serial.println("Record temp");
  recordTemperature();
  Serial.println("Going to sleep");
  esp_deep_sleep(recordIntervalMinutes*60*1000000);
  Serial.println("Should not be displayed");
}

// Set system time from phone time and time zone
void setSystemTimeFromPhone(String phoneTime, int timezoneOffset) {
  struct tm tm;
  if (strptime(phoneTime.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == NULL) {
    Serial.println("Failed to parse phone time");
    return;
  }

  time_t t = mktime(&tm);
  // Adjust for time zone offset (in minutes)
  t -= timezoneOffset * 60;
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  Serial.println("System time updated to UTC: " + phoneTime + ", with offset: " + String(timezoneOffset) + " minutes");
}

String readGPSFromFile() {
  File file = SPIFFS.open("/data.txt", "r");
  if (!file) {
    return "No GPS coordinates set.";
  }

  String gpsLine = file.readStringUntil('\n'); // Read the first line
  file.close();
  return gpsLine;
}

void saveGPSCoordinates(String gps) {
  File file = SPIFFS.open("/data.txt", "r");
  String existingData = "";

  // Skip the first line (GPS line)
  if (file) {
    file.readStringUntil('\n'); // Skip GPS line
    existingData = file.readString(); // Read the rest of the file
    file.close();
  }

  // Write updated file with new GPS coordinates on the first line
  file = SPIFFS.open("/data.txt", "w");
  if (file) {
    file.println(gps); // Write GPS coordinates
    file.print(existingData); // Append remaining data
    file.close();
  }
}


// Handle the main page
void handleRoot() {
  // Read the GPS coordinates from the file
  String gpsCoords = readGPSFromFile();

  // Retrieve the last measurement from flash memory
  String lastMeasurement = getLastMeasurement();

  // Retrieve the measurement counter from flash memory
  int measurementCounter = getMeasurementCounter();  // Function to read the counter from flash


  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <title>Retrieve Data</title>
      <style>
        /* Increase the size of the buttons */
        button {
          font-size: 18px;
          padding: 15px 25px;
          margin: 10px;
          cursor: pointer;
        }
        
        /* Increase the size of the input fields */
        input[type="number"] {
          font-size: 18px;
          padding: 10px;
          width: 200px;
          margin: 10px;
        }

        /* Increase the size of the displayed data */
        pre {
          font-size: 18px;
          white-space: pre-wrap;
          word-wrap: break-word;
        }

        /* Style for headings */
        h1, h2 {
          font-size: 24px;
          margin-bottom: 15px;
        }

        p {
          font-size: 18px;
          margin-bottom: 10px;
        }
      </style>
    </head>
    <body>
      <h1>Retrieve Data</h1>
      <p>Temperature data is being logged.</p>
      <p>Sensor ID: )rawliteral" + String(sensorID) + R"rawliteral(</p>
      <button onclick="sendTime()">Set Time from Phone</button>
      <button onclick="setGPS()">Set GPS Coordinates</button>
      <button onclick="window.location.href='/download'">Download Data</button>
      <button onclick="deleteData()">Delete Data</button>

      <h2>Adjust Record Interval</h2>
      <p>Enter time between records (in minutes):</p>
      <input type="number" id="interval" value=")rawliteral" + String(recordIntervalMinutes) + R"rawliteral(" min="1">
      <button onclick="setRecordInterval()">Set Interval</button>

      <h2>GPS Coordinates</h2>
      <p id="gps-coords">)rawliteral" + gpsCoords + R"rawliteral(</p>

      <h2>Measurement Counter</h2>
      <p>Measurement Count: )rawliteral" + String(measurementCounter) + R"rawliteral(</p>

      <h2>Last Recorded Data</h2>
      <pre>)rawliteral";
  html += lastMeasurement; // Add the last measurement
  html += R"rawliteral(</pre>

      <!-- "Go" button -->
      <h2>Start Measurements</h2>
      <button id="goButton" onclick="confirmStart()">Go</button>

      <script>
        function sendTime() {
          const now = new Date();
          const phoneTime = now.toISOString();
          const timezoneOffset = now.getTimezoneOffset();
          const xhr = new XMLHttpRequest();
          xhr.open("POST", "/set-time", true);
          xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
          xhr.send("time=" + encodeURIComponent(phoneTime) + "&offset=" + timezoneOffset);
          xhr.onload = function() {
            alert(xhr.responseText);
          };
        }

        function setGPS() {
          const gpsCoords = prompt("Enter GPS coordinates (latitude, longitude):");
          if (gpsCoords) {
            const xhr = new XMLHttpRequest();
            xhr.open("POST", "/set-gps", true);
            xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
            xhr.send("gps=" + encodeURIComponent(gpsCoords));
            xhr.onload = function() {
              alert(xhr.responseText);
              document.getElementById("gps-coords").innerText = gpsCoords;
            };
          }
        }

        function deleteData() {
          if (confirm("Are you sure you want to delete all data?")) {
            const xhr = new XMLHttpRequest();
            xhr.open("GET", "/delete", true);
            xhr.onload = function() {
              alert(xhr.responseText);
              location.reload();
            };
            xhr.send();
          }
        }

        function setRecordInterval() {
          const interval = document.getElementById("interval").value;
          if (interval >= 1) {
            const xhr = new XMLHttpRequest();
            xhr.open("POST", "/set-interval", true);
            xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
            xhr.send("interval=" + interval);
            xhr.onload = function() {
              alert(xhr.responseText);
            };
          } else {
            alert("Interval must be at least 1 minute.");
          }
        }

        function confirmStart() {
          if (confirm("Lancer les mesures?")) {
            // If the user confirms, call the recordTemperatureAndSleep function
            const xhr = new XMLHttpRequest();
            xhr.open("GET", "/start-measurements", true);  // This will call your recordTemperatureAndSleep function on the server
            xhr.onload = function() {
              alert("Measurements started.");
            };
            xhr.send();
          }
        }
      </script>
    </body>
    </html>
  )rawliteral";

  // Send the HTML with UTF-8 encoding
  server.send(200, "text/html; charset=utf-8", html);
}




void handleSetInterval() {
  if (server.hasArg("interval")) {
    int newInterval = server.arg("interval").toInt();
    if (newInterval >= 1) {
      recordIntervalMinutes = newInterval;
      server.send(200, "text/plain", "Interval updated to " + String(recordIntervalMinutes) + " minute(s).");
    } else {
      server.send(400, "text/plain", "Invalid interval. Must be at least 1 minute.");
    }
  } else {
    server.send(400, "text/plain", "Interval not provided.");
  }
}

// Handle time setting from phone
void handleSetTime() {
  if (server.hasArg("time") && server.hasArg("offset")) {
    String phoneTime = server.arg("time");
    int timezoneOffset = server.arg("offset").toInt();
    setSystemTimeFromPhone(phoneTime, timezoneOffset);
    server.send(200, "text/plain", "Time successfully updated: " + phoneTime + ", Offset: " + String(timezoneOffset) + " minutes");
  } else {
    server.send(400, "text/plain", "Missing 'time' or 'offset' parameter");
  }
}

void handleSetGPS() {
  if (server.hasArg("gps")) {
    String gpsData = server.arg("gps");

    // Save the GPS coordinates to a file in SPIFFS
    File file = SPIFFS.open("/gps.txt", "w");
    if (file) {
      saveGPSCoordinates(gpsData);
      file.close();
      server.send(200, "text/plain", "GPS coordinates saved: " + gpsData);
    } else {
      server.send(500, "text/plain", "Failed to save GPS coordinates.");
    }
  } else {
    server.send(400, "text/plain", "No GPS data received.");
  }
}

void handleDelete() {
  // Delete the data file
  if (SPIFFS.exists("/data.txt")) {
    saveLastMeasurement("No_data");
    saveCounterToFlash(0);
    MeasurementCounter = 0;
    SPIFFS.remove("/data.txt");
    Serial.println("Data deleted.");
  }
  
  // Redirect back to the root page after deletion
  server.sendHeader("Location", "/");
  server.send(303);  // HTTP 303: See Other
}

// Handle file download
void handleFileDownload() {
  const char* filename = "/data.txt";  // The file to be downloaded
  File file = SPIFFS.open(filename, FILE_READ);

  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  // Assume sensorID is globally defined or fetched from somewhere
  String downloadFileName = sensorID + "_data.txt";

  // Set headers for file download
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadFileName + "\"");
  server.sendHeader("Connection", "close");

  // Stream the file content
  size_t sent = server.streamFile(file, "application/octet-stream");
  file.close();

  Serial.printf("Sent %d bytes for download as '%s'\n", sent, downloadFileName.c_str());
}
