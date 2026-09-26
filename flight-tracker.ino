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

// Global client for making secure web requests. It is kept connected between
// requests: a fresh TCP+TLS handshake costs several seconds on the UNO R4,
// which was most of the per-request delay.
WiFiSSLClient client;

// Bound on any single network stall so a hung socket can't freeze the board.
const unsigned long HTTP_TIMEOUT_MS = 10000;
// Cap on the captured body, to protect the R4's 32 KB of SRAM.
const size_t MAX_RESPONSE_BYTES = 16384;

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
  Serial.println("Connecting to WiFi...");

  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(WIFI_SSID);
    status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Poll instead of a fixed delay so setup returns as soon as we are online.
    unsigned long attemptStart = millis();
    while (status != WL_CONNECTED && millis() - attemptStart < 15000) {
      delay(250);
      status = WiFi.status();
    }
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// Opens the TLS connection once and reuses it across requests. Reconnects
// automatically whenever the server (or idle timeout) has closed it.
bool ensureApiConnection() {
  if (client.connected()) return true;

  client.stop();
  client.setTimeout(HTTP_TIMEOUT_MS);

  if (WiFi.status() != WL_CONNECTED) connectToWiFi();

  Serial.println("Connecting to API host...");
  return client.connect(API_HOST, 443);
}

void appendBody(String& body, const uint8_t* buf, int n) {
  size_t room = MAX_RESPONSE_BYTES - body.length();
  if (n > (int)room) n = room;
  if (n > 0) body.concat((const char*)buf, n);
}

// Helper function to make the API requests and get the JSON body
String makeApiRequest(const String& endpoint) {
  unsigned long start = millis();
  Serial.println("\nRequest: " + endpoint);

  String body;
  body.reserve(4096); // Pre-allocate: per-byte += without reserve reallocs every byte.

  if (!ensureApiConnection()) {
    Serial.println("Connection to server failed!");
    lcd.clear();
    lcd.print("Connect failed");
    delay(1500);
    return body;
  }

  client.print("GET ");
  client.print(endpoint);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(API_HOST);
  client.print("X-RapidAPI-Key: ");
  client.println(API_KEY);
  client.print("X-RapidAPI-Host: ");
  client.println(API_HOST);
  client.println("Connection: keep-alive");
  client.println();

  // --- Status line and headers ---
  int statusCode = 0;
  long contentLength = -1;
  bool chunked = false;
  bool keepAlive = true; // HTTP/1.1 default
  bool headersDone = false;
  unsigned long deadline = millis() + HTTP_TIMEOUT_MS;

  while (client.connected() && millis() < deadline) {
    String line = client.readStringUntil('\n');
    if (line.length() == 0) break; // stalled
    if (line == "\r") { headersDone = true; break; }

    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("http/")) {
      statusCode = lower.substring(9, 12).toInt();
    } else if (lower.startsWith("content-length:")) {
      contentLength = lower.substring(15).toInt();
    } else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (lower.startsWith("connection:") && lower.indexOf("close") >= 0) {
      keepAlive = false;
    }
  }
  Serial.println("HTTP " + String(statusCode));

  // No length and not chunked: the only end-of-body signal is the server
  // closing, so the socket can't be reused.
  if (!chunked && contentLength < 0) keepAlive = false;

  // --- Body ---
  bool complete = headersDone;
  deadline = millis() + HTTP_TIMEOUT_MS;
  uint8_t buf[128];

  if (complete && chunked) {
    // Decode chunk framing so the connection stays reusable.
    while (millis() < deadline) {
      String sizeLine = client.readStringUntil('\n');
      if (sizeLine.length() == 0) { complete = false; break; }
      long chunkSize = strtol(sizeLine.c_str(), NULL, 16);
      if (chunkSize <= 0) {
        // Consume trailer lines so nothing is left for the next request.
        while (client.connected()) {
          String trailer = client.readStringUntil('\n');
          if (trailer.length() == 0 || trailer == "\r") break;
        }
        break;
      }
      while (chunkSize > 0 && millis() < deadline) {
        int avail = client.available();
        if (avail <= 0) { delay(1); continue; }
        int n = client.read(buf, min(avail, (int)sizeof(buf)));
        if (n <= 0) { complete = false; break; }
        appendBody(body, buf, n);
        chunkSize -= n;
      }
      client.readStringUntil('\n'); // CRLF after each chunk
    }
  } else if (complete) {
    while (millis() < deadline) {
      if (contentLength >= 0 && (long)body.length() >= contentLength) break;
      int avail = client.available();
      if (avail <= 0) {
        if (!client.connected()) break; // server closed the connection
        delay(1);
        continue;
      }
      int n = client.read(buf, min(avail, (int)sizeof(buf)));
      if (n <= 0) break;
      appendBody(body, buf, n);
    }
    if (contentLength >= 0 && (long)body.length() < contentLength) complete = false;
  }

  // Only keep the socket open if the whole response was consumed and the
  // server agreed to keep-alive; anything else would corrupt the next read.
  if (!(complete && keepAlive)) client.stop();

  Serial.println("Body: " + String(body.length()) + " bytes in " + String(millis() - start) + "ms");
  return body;
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

// Extracts an airport code from a value that is either a string ("SYD")
// or an object holding one ({"iata":"SYD"} / {"icao":"YSSY"}).
String airportCode(JSONVar value) {
  if (JSON.typeof(value) == "string") return (const char*)value;
  if (JSON.typeof(value) == "object") {
    if (JSON.typeof(value["iata"]) == "string") return (const char*)value["iata"];
    if (JSON.typeof(value["icao"]) == "string") return (const char*)value["icao"];
    if (JSON.typeof(value["code"]) == "string") return (const char*)value["code"];
  }
  return "";
}

// Builds the "AAA -> BBB" route straight from a boundary-list entry when the
// data is already there, skipping the extra API call entirely. Returns ""
// when the list data doesn't carry route info.
String routeFromListEntry(JSONVar& flight) {
  if (JSON.typeof(flight["route"]) == "string") {
    String route = (const char*)flight["route"];
    route.replace("⟶", "->");
    return route;
  }

  String origin = airportCode(flight["origin"]);
  String destination = airportCode(flight["destination"]);
  if (origin.length() == 0 && JSON.typeof(flight["airport"]) == "object") {
    origin = airportCode(flight["airport"]["origin"]);
    destination = airportCode(flight["airport"]["destination"]);
  }

  if (origin.length() > 0 && destination.length() > 0) {
    return origin + " -> " + destination;
  }
  return "";
}

// Fallback for when the boundary list doesn't include route info:
// resolve the callsign via the search endpoint.
String lookupRouteForCallsign(const String& callsign) {
  String searchResponse = makeApiRequest("/flights/search?query=" + callsign + "&limit=10");
  if (searchResponse.length() == 0) return "";

  JSONVar searchResult = JSON.parse(searchResponse);
  if (JSON.typeof(searchResult["results"]) == "undefined" || searchResult["results"].length() == 0) {
    Serial.println("Search for " + callsign + " returned no results.");
    return "";
  }

  for (int j = 0; j < searchResult["results"].length(); j++) {
    JSONVar currentResult = searchResult["results"][j];

    if (JSON.typeof(currentResult["detail"]) != "undefined" && String((const char*)currentResult["type"]) == "live") {
      String route = (const char*)currentResult["detail"]["route"];
      route.replace("⟶", "->");
      Serial.println("Found live flight for " + callsign + ": " + route);
      return route;
    }
  }

  Serial.println("Could not find a 'live' entry for " + callsign);
  return "";
}

void fetchAndDisplayFlights() {
  lcd.backlight();
  lcd.clear();
  lcd.print("Fetching nearby");
  lcd.setCursor(0, 1);
  lcd.print("flights...");

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
        JSONVar flight = response["flightsList"][i];
        String callsign = (const char*)flight["callsign"];
        if (callsign == "" || callsign == "N/A") {
          Serial.println("Skipping flight with no callsign.");
          continue;
        }

        // Dump the first entry once so the available fields can be seen in
        // the Serial monitor (e.g. whether route/origin/destination came free).
        if (i == 0) {
          Serial.println("First entry: " + JSON.stringify(flight));
        }

        String route = routeFromListEntry(flight);

        if (route.length() == 0) {
          lcd.clear();
          String flightsFoundText = "Found " + String(flightCount) + pluralize(" flight", flightCount);
          lcd.print(flightsFoundText);
          lcd.setCursor(0, 1);
          lcd.print("fetching info...");

          route = lookupRouteForCallsign(callsign);
        }

        if (route.length() > 0) {
          Serial.println(callsign + ": " + route);
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print(callsign + ":");
          lcd.setCursor(0, 1);
          scrollText(route, 1, 15000);
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
