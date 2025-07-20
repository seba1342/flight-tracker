// LIBRARIES
#include <WiFiS3.h>
#include <Arduino_JSON.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

// Checkout the README on how to setup your SECRETS
// --- WiFi Credentials ---
char WIFI_SSID[] = SECRET_SSID;
char WIFI_PASSWORD[] = SECRET_PASS;

// --- RapidAPI Credentials ---
const char* API_KEY = SECRET_API_KEY;
const char* API_HOST = SECRET_API_HOST;

// --- Hardware Pin Configuration ---
const int BUTTON_PIN = 4;

// --- LCD Configuration ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Flight Area Bounding Box ---
const char* BBOX_BL_LAT = SECRET_BBOX_BL_LAT; // Bottom-Left Latitude
const char* BBOX_BL_LON = SECRET_BBOX_BL_LON; // Bottom-Left Longitude
const char* BBOX_TR_LAT = SECRET_BBOX_TR_LAT; // Top-Right Latitude
const char* BBOX_TR_LON = SECRET_BBOX_TR_LON; // Top-Right Longitude

// Global client for making secure web requests
WiFiSSLClient client;

void setup() {
  Serial.begin(9600);
  while (!Serial);

  // Initialize the LCD screen
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("     __|__     ");
  lcd.setCursor(0, 1);
  lcd.print("*--o--(_)--o--*");
  delay(1000);

  // Configure the button switch pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Connect to WiFi
  connectToWiFi();

  lcd.clear();
  lcd.print("Flight Tracker");
  lcd.setCursor(0, 1);
  lcd.print("Press button.");
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // Debounce
    fetchAndDisplayFlights();
    while (digitalRead(BUTTON_PIN) == LOW); // Wait for release
  }
}

void connectToWiFi() {
  // lcd.clear();
  // lcd.print("Connecting WiFi");
  Serial.println("Connecting to WiFi...");

  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(WIFI_SSID);
    status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // wait 10 seconds for connection:
    delay(10000);
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// Helper function to make the API requests and get the JSON body
String makeApiRequest(String endpoint) {
  Serial.println("\nStarting request to: " + endpoint);
  String responseBody = "";

  if (client.connect(API_HOST, 443)) {
    Serial.println("Connected to server.");
    client.println("GET " + endpoint + " HTTP/1.1");
    client.println("Host: " + String(API_HOST));
    client.println("X-RapidAPI-Key: " + String(API_KEY));
    client.println("X-RapidAPI-Host: " + String(API_HOST));
    client.println("Connection: close");
    client.println();

    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r") {
        Serial.println("Headers received.");
        break;
      }
    }

    while (client.available()) {
      char c = client.read();
      responseBody += c;
    }
    client.stop();
    Serial.println("Connection closed.");
  } else {
    Serial.println("Connection to server failed!");
    lcd.clear();
    lcd.print("Connect failed");
    delay(2000);
  }
  return responseBody;
}

/**
 * Prints text to the LCD on a specified row. If the text is longer
 * than the display width (16 chars), it will scroll the text in a
 * continuous marquee style with a 3-space gap.
 * @param textToScroll The String of text to display.
 * @param row The LCD row to print on (0 for the top, 1 for the bottom).
 * @param totalDisplayTime The total time in milliseconds to display/scroll the text.
 */
void scrollText(String textToScroll, int row, int totalDisplayTime) {
  const int screenWidth = 16;
  const int scrollSpeed = 250;
  bool shouldDelay = totalDisplayTime > 0;

  lcd.setCursor(0, row);

  // If the text is short enough to fit, just print it and wait.
  if (textToScroll.length() <= screenWidth) {
    lcd.print(textToScroll);
    if (shouldDelay) {
      delay(totalDisplayTime);
    }
    return;
  }

  lcd.print(textToScroll);
  delay(1500);

  const String gap = "   ";
  String marqueeText = textToScroll + gap;
  int marqueeLength = marqueeText.length();

  unsigned long startTime = millis();
  int position = 0;

  while (millis() - startTime < totalDisplayTime) {
    String sub = "";
    for (int i = 0; i < screenWidth; i++) {
      sub += marqueeText.charAt((position + i) % marqueeLength);
    }

    // Print the generated substring to the screen
    lcd.setCursor(0, row);
    lcd.print(sub);

    // Wait for the defined scroll speed
    delay(scrollSpeed);

    // Move to the next position for the next frame
    position++;

    // If we've scrolled through the entire virtual string, wrap back to the start.
    // This prevents the 'position' variable from growing infinitely large.
    if (position >= marqueeLength) {
      position = 0;
    }

    // Exit if our total allotted time is up
    if (shouldDelay && millis() - startTime >= totalDisplayTime) {
      break;
    }
  }
}

String pluralize(String word, int count) {
  return word + (count <= 1 ? "" : "s");
}

void fetchAndDisplayFlights() {
  lcd.backlight();
  lcd.clear();
  lcd.print("Fetching nearby");
  lcd.setCursor(0, 1);
  lcd.print("flights...");

  https://flight-radar1.p.rapidapi.com/flights/v2/list-in-boundary?south=-37.791337&west=144.654959&north=-37.706839&east=144.868011&limit=300&dataSource=ADSB%2CMLAT%2CFLARM%2CFAA%2CSATELLITE%2CUAT%2CSPIDERTRACKS%2CAUS%2COTHER_DATA_SOURCE%2CESTIMATED&service=PASSENGER%2CCARGO%2CMILITARY_AND_GOVERNMENT%2CBUSINESS_JETS%2CGENERAL_AVIATION%2CHELICOPTERS%2CLIGHTER_THAN_AIR%2CDRONES%2COTHER_SERVICE%2CNON_CATEGORIZED%2CGLIDERS%2CGROUND_VEHICLES&trafficType=ALL&stats=true

  String boundaryEndpoint =
    String("/flights/v2/list-in-boundary?") + "south=" + String(BBOX_BL_LAT) + "&west=" + String(BBOX_BL_LON) + "&north=" + String(BBOX_TR_LAT) + "&east=" + String(BBOX_TR_LON) + "&limit=10&dataSource=ADSB%2CMLAT%2CFLARM%2CFAA%2CSATELLITE%2CUAT%2CSPIDERTRACKS%2CAUS%2COTHER_DATA_SOURCE%2CESTIMATED&service=PASSENGER%2CCARGO%2CMILITARY_AND_GOVERNMENT%2CBUSINESS_JETS&trafficType=AIRBORNE_ONLY";

  String boundaryResponse = makeApiRequest(boundaryEndpoint);

  if (boundaryResponse.length() > 0) {
    JSONVar response = JSON.parse(boundaryResponse);

    if (JSON.typeof(response["flightsList"]) != "undefined" && response["flightsList"].length() > 0) {
      int flightCount = response["flightsList"].length();
      Serial.print(flightCount);
      Serial.println(" flightsList found. Getting details...");

      for (int i = 0; i < flightCount; i++) {
        String callsign = (const char*) response["flightsList"][i]["callsign"];
        if (callsign == "" || callsign == "N/A") {
          Serial.println("Skipping flight with no callsign.");
          continue;
        }

        lcd.clear();
        String flightsFoundText = "Found " + String(flightCount) + pluralize(" flight", flightCount);
        lcd.print(flightsFoundText);
        lcd.setCursor(0, 1);
        lcd.print("fetching info...");

        String searchEndpoint = "/flights/search?query=" + callsign + "&limit=10";
        String searchResponse = makeApiRequest(searchEndpoint);

        if (searchResponse.length() > 0) {
          JSONVar searchResult = JSON.parse(searchResponse);

          if (JSON.typeof(searchResult["results"]) != "undefined" && searchResult["results"].length() > 0) {
            bool liveFlightFound = false;
            for (int j = 0; j < searchResult["results"].length(); j++) {
              JSONVar currentResult = searchResult["results"][j];

              if (JSON.typeof(currentResult["detail"]) != "undefined" && String((const char*)currentResult["type"]) == "live") {
                Serial.println("Found live flight for " + callsign);
                String route = (const char*) currentResult["detail"]["route"];
                route.replace("⟶", "->");

                Serial.println(route);

                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print(callsign + ":");
                lcd.setCursor(0, 1);
                scrollText(route, 1, 15000);

                liveFlightFound = true;
                break;
              }
            }

            if (!liveFlightFound) {
              Serial.println("Could not find a 'live' entry for " + callsign);
            }

          } else {
            Serial.println("Search for " + callsign + " returned no results.");
          }
        }
      }
    } else {
      Serial.println("No flights found in the boundary.");
      lcd.clear();
      lcd.print("No flights");
      lcd.setCursor(0, 1);
      lcd.print("nearby :(");
      delay(4000);
    }
  } else {
    Serial.println("Failed to get response for boundary list.");
    lcd.clear();
    lcd.print("API Error");
    delay(3000);
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("     __|__     ");
  lcd.setCursor(0, 1);
  lcd.print("*--o--(_)--o--*");
  delay(5000);
  lcd.noBacklight();
}
