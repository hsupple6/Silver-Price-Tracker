#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

// Display setup
TFT_eSPI tft = TFT_eSPI();

// Access Point credentials
const char* ap_ssid = "Silver Tracka";
const char* ap_password = ""; // No password for easier access

// Web server for configuration
WebServer server(80);

// Preferences for storing WiFi credentials
Preferences preferences;

// WiFi credentials (loaded from storage)
String ssid = "";
String password = "";
bool wifiConfigured = false;
bool apMode = true;

// API endpoint
const char* apiUrl = "https://data.silv.app/commodities.json";

// Price history structure
struct PricePoint {
  double price;
  unsigned long time; // milliseconds since start
};

// State variables
String oldPriceStr = "";
String oldAskStr = "";
String oldBidStr = "";
double oldPrice = 0.0;
double oldAsk = 0.0;
double oldBid = 0.0;
bool firstUpdate = true;

// Price history (using array to avoid std::vector issues)
#define MAX_HISTORY_POINTS 200
PricePoint priceHistory[MAX_HISTORY_POINTS];
int priceHistoryCount = 0;
unsigned long graphStartTime = 0;
bool graphStartTimeSet = false;

// Y-axis bounds
double graphMinPrice = 0.0;
double graphMaxPrice = 0.0;
bool graphBoundsInitialized = false;

// Reference price
const double referencePrice = 61.00;

// Update timing
unsigned long lastFetchTime = 0;
const unsigned long fetchInterval = 3000; // 3 seconds

// Deep sleep and button
const int buttonPin = 0;
const int backlightPin = 12;
const unsigned long activeTime = 10000; // 10 seconds in milliseconds
const unsigned long sleepInterval = 5 * 60 * 1000000ULL; // 5 minutes in microseconds
unsigned long wakeTime = 0;
bool shouldSleep = false;

// Font sizes
#define FONT_TINY 1
#define FONT_SMALL 2
#define FONT_SMEDIUM 3
#define FONT_MEDIUM 5
#define FONT_LARGE 6

void setup() {
  Serial.begin(115200);
  
  // Initialize display
  tft.init();
  tft.setRotation(3); // Landscape mode
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  
  // Configure button pin
  pinMode(buttonPin, INPUT_PULLUP);
  
  // Configure backlight pin (but don't turn it on yet)
  pinMode(backlightPin, OUTPUT);
  
  // Check wake reason
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool wokeFromSleep = (wakeReason == ESP_SLEEP_WAKEUP_EXT0);
  bool wokeFromTimer = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);
  
  // Initialize preferences
  preferences.begin("wifi", false);
  
  // Try to load saved WiFi credentials
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  
  // Load price history from storage
  loadPriceHistory();
  
  // If we woke from timer, fetch data and go back to sleep (no display, no backlight)
  if (wokeFromTimer && ssid.length() > 0) {
    Serial.println("Woke from timer - fetching data...");
    
    // Keep backlight OFF for timer wakeups
    
    // Minimal initialization for background fetch (no display needed)
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected - fetching price...");
      
      // Fetch and append price data
      fetchAndAppendPrice();
      
      // Disconnect WiFi
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    } else {
      Serial.println("\nFailed to connect - clearing WiFi credentials");
      clearWiFiCredentials();
    }
    
    // Go back to sleep immediately (no display)
    goToDeepSleep();
    return; // Never reaches here, but good practice
  }
  
  // Turn on backlight for all non-timer wakeups
  digitalWrite(backlightPin, HIGH);
  
  // If we woke from sleep via button and have WiFi credentials, try quick reconnect
  if (wokeFromSleep && ssid.length() > 0) {
    Serial.println("Woke from deep sleep via button press");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected to WiFi!");
      wifiConfigured = true;
      apMode = false;
      configTime(0, 0, "pool.ntp.org");
    } else {
      // If reconnect fails, clear credentials and start AP mode
      Serial.println("\nFailed to reconnect - clearing WiFi credentials");
      clearWiFiCredentials();
      startAPMode();
      return;
    }
    
    // Adjust graphStartTime since millis() resets after deep sleep
    // The stored times are relative, so we reset graphStartTime to 0 (current millis)
    // But we keep the loaded history times as-is since they're already relative
    if (graphStartTimeSet) {
      graphStartTime = 0; // millis() is now 0 after wake
      // History times are already relative, so they remain valid
    }
    
    // Record wake time and proceed to normal operation
    wakeTime = millis();
    shouldSleep = true;
    return;
  }
  
  // Normal startup (not from deep sleep)
  if (ssid.length() > 0) {
    // Try to connect to saved WiFi
    Serial.print("Connecting to saved WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi!");
      wifiConfigured = true;
      apMode = false;
      
      // Set timezone
      configTime(0, 0, "pool.ntp.org");
      
      // Initial display
      tft.fillScreen(TFT_BLACK);
      drawText(160, 120, "Connected!", FONT_MEDIUM, true);
      delay(1000);
      
      // Record wake time for deep sleep countdown
      wakeTime = millis();
      shouldSleep = true;
    } else {
      Serial.println("\nFailed to connect to saved WiFi - clearing credentials");
      clearWiFiCredentials();
      startAPMode();
    }
  } else {
    Serial.println("No saved WiFi credentials. Starting AP mode.");
    startAPMode();
  }
}

void clearWiFiCredentials() {
  preferences.remove("ssid");
  preferences.remove("password");
  ssid = "";
  password = "";
  Serial.println("WiFi credentials cleared");
}

void savePriceHistory() {
  // Save count
  preferences.putInt("priceCount", priceHistoryCount);
  
  // Save each price point
  for (int i = 0; i < priceHistoryCount && i < MAX_HISTORY_POINTS; i++) {
    char key[20];
    sprintf(key, "price_%d", i);
    preferences.putDouble(key, priceHistory[i].price);
    
    sprintf(key, "time_%d", i);
    preferences.putULong(key, priceHistory[i].time);
  }
  
  // Save graph state
  preferences.putULong("graphStart", graphStartTime);
  preferences.putBool("graphStartSet", graphStartTimeSet);
  preferences.putDouble("graphMin", graphMinPrice);
  preferences.putDouble("graphMax", graphMaxPrice);
  preferences.putBool("graphBounds", graphBoundsInitialized);
  
  Serial.print("Saved price history: ");
  Serial.print(priceHistoryCount);
  Serial.println(" points");
}

void loadPriceHistory() {
  // Load count
  priceHistoryCount = preferences.getInt("priceCount", 0);
  
  if (priceHistoryCount > 0 && priceHistoryCount <= MAX_HISTORY_POINTS) {
    // Load each price point
    for (int i = 0; i < priceHistoryCount; i++) {
      char key[20];
      sprintf(key, "price_%d", i);
      priceHistory[i].price = preferences.getDouble(key, 0.0);
      
      sprintf(key, "time_%d", i);
      priceHistory[i].time = preferences.getULong(key, 0);
    }
    
    // Load graph state
    graphStartTime = preferences.getULong("graphStart", 0);
    graphStartTimeSet = preferences.getBool("graphStartSet", false);
    graphMinPrice = preferences.getDouble("graphMin", 0.0);
    graphMaxPrice = preferences.getDouble("graphMax", 0.0);
    graphBoundsInitialized = preferences.getBool("graphBounds", false);
    
    Serial.print("Loaded price history: ");
    Serial.print(priceHistoryCount);
    Serial.println(" points");
    
    // Set display values from last price in history
    if (priceHistoryCount > 0) {
      oldPrice = priceHistory[priceHistoryCount - 1].price;
      oldAsk = oldPrice; // Use price as fallback
      oldBid = oldPrice; // Use price as fallback
      // Format strings will be set on first fetchAndUpdate() call
      firstUpdate = false; // We have data to display
    }
  } else {
    // No saved history or invalid count
    priceHistoryCount = 0;
    graphStartTimeSet = false;
    graphBoundsInitialized = false;
  }
}

void startAPMode() {
  apMode = true;
  wifiConfigured = false;
  
  // Start Access Point
  WiFi.mode(WIFI_AP);
  
  // Configure AP IP address to 10.0.0.1
  IPAddress local_IP(10, 0, 0, 1);
  IPAddress gateway(10, 0, 0, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/configure", HTTP_POST, handleConfigure);
  server.onNotFound(handleRoot);
  
  server.begin();
  Serial.println("Web server started");
  
  // Display instructions
  showAPInstructions();
}

void loop() {
  // Handle web server if in AP mode (don't sleep in AP mode)
  if (apMode) {
    server.handleClient();
    // Keep showing instructions
    showAPInstructions();
    delay(100);
    return;
  }
  
  // Check if we should go to deep sleep (after 10 seconds of activity)
  if (shouldSleep && wifiConfigured) {
    unsigned long currentTime = millis();
    if (currentTime - wakeTime >= activeTime) {
      // Time to sleep
      goToDeepSleep();
    }
  }
  
  // Normal operation when WiFi is configured
  unsigned long currentTime = millis();
  
  // Fetch data every 3 seconds
  if (currentTime - lastFetchTime >= fetchInterval) {
    lastFetchTime = currentTime;
    fetchAndUpdate();
  }
  
  // Update display continuously
  drawDisplay();
  
  delay(100); // Small delay to prevent overwhelming the ESP32
}

void fetchAndUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  HTTPClient http;
  http.begin(apiUrl);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    
    // Parse JSON
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);
    
    if (doc.containsKey("commodities") && doc["commodities"].containsKey("silver")) {
      JsonObject silver = doc["commodities"]["silver"];
      
      double price = silver["price"].as<double>();
      double ask = silver.containsKey("ask") ? silver["ask"].as<double>() : price;
      double bid = silver.containsKey("bid") ? silver["bid"].as<double>() : price;
      
      // Format prices
      String priceStr = formatPrice(price);
      String askStr = formatPrice(ask);
      String bidStr = formatPrice(bid);
      
      // Check for changes
      if (!firstUpdate && (oldPriceStr != priceStr || oldAskStr != askStr || oldBidStr != bidStr)) {
        // Animation would trigger here if needed
      }
      
      // Update old values
      oldPriceStr = priceStr;
      oldAskStr = askStr;
      oldBidStr = bidStr;
      oldPrice = price;
      oldAsk = ask;
      oldBid = bid;
      
      // Add to price history with compression
      unsigned long currentMillis = millis();
      if (!graphStartTimeSet) {
        graphStartTime = currentMillis;
        graphStartTimeSet = true;
      }
      
      // Calculate time relative to graph start, or continue from last point if history exists
      unsigned long newTime;
      if (priceHistoryCount > 0) {
        // Continue time sequence from last point (maintain relative times)
        // Use a small increment (3 seconds) for display updates
        newTime = priceHistory[priceHistoryCount - 1].time + 3000; // 3 seconds
      } else {
        // First point - start at 0
        newTime = 0;
      }
      
      bool priceChanged = false;
      if (priceHistoryCount > 0 && priceHistory[priceHistoryCount - 1].price == price) {
        // Same price - extend time (don't save)
        priceHistory[priceHistoryCount - 1].time = newTime;
      } else {
        // Different price - add new point
        priceChanged = true;
        if (priceHistoryCount < MAX_HISTORY_POINTS) {
          priceHistory[priceHistoryCount].price = price;
          priceHistory[priceHistoryCount].time = newTime;
          priceHistoryCount++;
        } else {
          // Shift array left to make room (remove oldest)
          for (int i = 0; i < MAX_HISTORY_POINTS - 1; i++) {
            priceHistory[i] = priceHistory[i + 1];
          }
          priceHistory[MAX_HISTORY_POINTS - 1].price = price;
          priceHistory[MAX_HISTORY_POINTS - 1].time = newTime;
        }
      }
      
      // Update Y-axis bounds
      if (!graphBoundsInitialized) {
        graphMinPrice = price;
        graphMaxPrice = price;
        graphBoundsInitialized = true;
      } else {
        if (price < graphMinPrice) graphMinPrice = price;
        if (price > graphMaxPrice) graphMaxPrice = price;
      }
      
      firstUpdate = false;
      
      // Save price history to persistent storage only if price changed
      if (priceChanged) {
        savePriceHistory();
      }
    }
  } else {
    http.end();
  }
}

void fetchAndAppendPrice() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  HTTPClient http;
  http.begin(apiUrl);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    
    // Parse JSON
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);
    
    if (doc.containsKey("commodities") && doc["commodities"].containsKey("silver")) {
      JsonObject silver = doc["commodities"]["silver"];
      double price = silver["price"].as<double>();
      
      // Initialize graph start time if needed
      if (!graphStartTimeSet) {
        graphStartTime = 0;
        graphStartTimeSet = true;
      }
      
      // Calculate time for this entry (5 minutes = 300000 ms after last entry)
      unsigned long newTime;
      if (priceHistoryCount > 0) {
        newTime = priceHistory[priceHistoryCount - 1].time + 300000; // 5 minutes in ms
      } else {
        newTime = 0;
      }
      
      // Add to price history
      bool priceChanged = false;
      if (priceHistoryCount > 0 && priceHistory[priceHistoryCount - 1].price == price) {
        // Same price - extend time (don't save)
        priceHistory[priceHistoryCount - 1].time = newTime;
      } else {
        // Different price - add new point
        priceChanged = true;
        if (priceHistoryCount < MAX_HISTORY_POINTS) {
          priceHistory[priceHistoryCount].price = price;
          priceHistory[priceHistoryCount].time = newTime;
          priceHistoryCount++;
        } else {
          // Shift array left to make room (remove oldest)
          for (int i = 0; i < MAX_HISTORY_POINTS - 1; i++) {
            priceHistory[i] = priceHistory[i + 1];
          }
          priceHistory[MAX_HISTORY_POINTS - 1].price = price;
          priceHistory[MAX_HISTORY_POINTS - 1].time = newTime;
        }
      }
      
      // Update Y-axis bounds
      if (!graphBoundsInitialized) {
        graphMinPrice = price;
        graphMaxPrice = price;
        graphBoundsInitialized = true;
      } else {
        if (price < graphMinPrice) graphMinPrice = price;
        if (price > graphMaxPrice) graphMaxPrice = price;
      }
      
      // Save price history to persistent storage only if price changed
      if (priceChanged) {
        savePriceHistory();
      }
      
      Serial.print("Price appended: $");
      Serial.println(price);
    }
  } else {
    http.end();
  }
}

void goToDeepSleep() {
  Serial.println("Going to deep sleep...");
  
  // Turn off backlight
  digitalWrite(backlightPin, LOW);
  
  // Turn off display
  tft.fillScreen(TFT_BLACK);
  
  // Configure button as wake source (wake on LOW when button pressed)
  rtc_gpio_pulldown_dis(GPIO_NUM_0);  // Disable pulldown
  rtc_gpio_pullup_en(GPIO_NUM_0);     // Enable pullup for RTC
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  
  // Configure timer wakeup (5 minutes)
  esp_sleep_enable_timer_wakeup(sleepInterval);
  
  // Disconnect WiFi to save power
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Go to deep sleep
  esp_deep_sleep_start();
  // Code never reaches here
}

void drawDisplay() {
  tft.fillScreen(TFT_BLACK);
  
  // Draw birthday countdown (top left)
  drawBirthdayCountdown();
  
  // Draw main price (top center)
  if (!oldPriceStr.isEmpty()) {
    drawText(160, 35, oldPriceStr, FONT_MEDIUM, true);
  }
  
  // Draw ask and bid (middle)
  int midX = 160;
  int midY = 80;
  if (!oldAskStr.isEmpty()) {
    drawText(midX - 60, midY, oldAskStr, FONT_SMALL, true);
    drawText(midX - 60, midY + 25, "Ask", FONT_SMALL, true);
  }
  if (!oldBidStr.isEmpty()) {
    drawText(midX + 60, midY, oldBidStr, FONT_SMALL, true);
    drawText(midX + 60, midY + 25, "Bid", FONT_SMALL, true);
  }
  
  // Draw price graph
  if (graphBoundsInitialized && priceHistoryCount >= 2) {
    drawPriceGraph(10, midY + 50, 300, 60);
  }
  
  // Draw unrealized gain (bottom)
  if (!firstUpdate) {
    double gain = oldPrice - referencePrice;
    String gainValue = formatGain(oldPrice, referencePrice);
    drawText(midX, 210, gainValue, FONT_SMALL, true);
    drawText(midX, 225, "Unrealized Gain", FONT_SMALL, true);
  }
}

void drawBirthdayCountdown() {
  int leftX = 10;
  int topY = 10;
  
  drawText(leftX, topY, "HLS Bday", FONT_SMALL, false);
  
  String countdown = getBirthdayCountdown();
  drawText(leftX, topY + 18, countdown, FONT_TINY, false);
}

String getBirthdayCountdown() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  int currentYear = timeinfo->tm_year + 1900;
  int currentMonth = timeinfo->tm_mon + 1;
  int currentDay = timeinfo->tm_mday;
  
  // Check if today is February 28
  if (currentMonth == 2 && currentDay == 28) {
    return "TODAY";
  }
  
  // Determine target year
  int targetYear = currentYear;
  if (currentMonth > 2 || (currentMonth == 2 && currentDay > 28)) {
    targetYear = currentYear + 1;
  }
  
  // Create target date (February 28)
  struct tm target_tm = *timeinfo;
  target_tm.tm_year = targetYear - 1900;
  target_tm.tm_mon = 1; // February (0-indexed)
  target_tm.tm_mday = 28;
  target_tm.tm_hour = 0;
  target_tm.tm_min = 0;
  target_tm.tm_sec = 0;
  
  time_t targetTime = mktime(&target_tm);
  double diffSeconds = difftime(targetTime, now);
  
  if (diffSeconds <= 0) {
    // Calculate for next year
    targetYear = currentYear + 1;
    target_tm.tm_year = targetYear - 1900;
    targetTime = mktime(&target_tm);
    diffSeconds = difftime(targetTime, now);
  }
  
  // Convert to long for modulo operations
  long totalSeconds = (long)diffSeconds;
  long days = totalSeconds / 86400;
  long hours = (totalSeconds % 86400) / 3600;
  long minutes = (totalSeconds % 3600) / 60;
  
  char buffer[20];
  sprintf(buffer, "%03ld-%02ld-%02ld", days, hours, minutes);
  return String(buffer);
}

void drawPriceGraph(int x, int y, int width, int height) {
  if (priceHistoryCount < 2) return;
  
  // Calculate Y-axis bounds
  double minPrice = graphMinPrice - 0.05;
  double maxPrice = graphMaxPrice + 0.05;
  double priceRange = maxPrice - minPrice;
  if (priceRange <= 0) return; // Safety check
  
  // Calculate time range
  unsigned long maxTime = priceHistory[priceHistoryCount - 1].time;
  bool useEvenSpacing = false;
  double timeRange = maxTime / 1000.0; // Convert to seconds
  
  // If all times are 0 or very small, space points evenly
  if (maxTime == 0 && priceHistoryCount > 1) {
    useEvenSpacing = true;
    timeRange = (priceHistoryCount - 1) * 1.0; // 1 second per point spacing
  } else if (timeRange < 0.1) {
    timeRange = 0.1;
  }
  
  // Draw border
  tft.drawRect(x, y, width, height, TFT_DARKGREY);
  
  // Draw Y-axis labels
  char minStr[20], maxStr[20];
  sprintf(minStr, "$%.2f", minPrice);
  sprintf(maxStr, "$%.2f", maxPrice);
  drawText(x + 2, y + height - 12, String(minStr), FONT_TINY, false);
  drawText(x + 2, y + 2, String(maxStr), FONT_TINY, false);
  
  // Draw graph line
  int prevX = -1, prevY = -1;
  for (int i = 0; i < priceHistoryCount; i++) {
    const PricePoint& point = priceHistory[i];
    
    // Calculate X position
    double timeRatio;
    if (useEvenSpacing) {
      // Space points evenly across the width
      timeRatio = (double)i / (priceHistoryCount - 1);
    } else if (timeRange > 0.001) {
      // Use actual time values
      timeRatio = (point.time / 1000.0) / timeRange;
      if (timeRatio > 1.0) timeRatio = 1.0; // Clamp to prevent overflow
      if (timeRatio < 0.0) timeRatio = 0.0;
    } else {
      // Fallback: space points evenly
      timeRatio = (double)i / (priceHistoryCount - 1);
    }
    int graphX = x + (int)(timeRatio * width);
    if (graphX < x) graphX = x;
    if (graphX > x + width) graphX = x + width;
    
    // Calculate Y position
    double priceRatio = (point.price - minPrice) / priceRange;
    int graphY = y + height - (int)(priceRatio * height);
    if (graphY < y) graphY = y;
    if (graphY > y + height) graphY = y + height;
    
    if (prevX >= 0 && prevY >= 0) {
      tft.drawLine(prevX, prevY, graphX, graphY, TFT_GREEN);
    }
    
    prevX = graphX;
    prevY = graphY;
  }
}

String formatPrice(double price) {
  char buffer[20];
  sprintf(buffer, "$%.2f", price);
  return String(buffer);
}

String formatGain(double current, double reference) {
  double gain = current - reference;
  char buffer[20];
  if (gain >= 0) {
    sprintf(buffer, "+$%.2f", gain);
  } else {
    sprintf(buffer, "$%.2f", gain);
  }
  return String(buffer);
}

void drawText(int x, int y, String text, int fontSize, bool center) {
  tft.setTextSize(fontSize);
  tft.setTextFont(1);
  
  if (center) {
    // Estimate text width (approximate: 6 pixels per character at size 1)
    int textWidth = text.length() * 6 * fontSize;
    x = x - textWidth / 2;
  }
  
  tft.setCursor(x, y);
  tft.print(text);
}

void showAPInstructions() {
  tft.fillScreen(TFT_BLACK);
  tft.setRotation(3);
  tft.setTextColor(TFT_WHITE);
  
  drawText(160, 30, "Connect to", FONT_SMALL, true);
  drawText(160, 60, "\"Silver Tracka\"", FONT_SMEDIUM, true);
  drawText(160, 90, "WiFi", FONT_SMALL, true);
  drawText(160, 130, "Then open", FONT_SMALL, true);
  drawText(160, 155, "10.0.0.1", FONT_SMEDIUM, true);
  drawText(160, 185, "in browser", FONT_SMALL, true);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Silver Tracka WiFi Setup</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; background: #1a1a1a; color: #fff; padding: 20px; }";
  html += "h1 { color: #c0c0c0; }";
  html += "form { max-width: 400px; margin: 0 auto; }";
  html += "input { width: 100%; padding: 10px; margin: 10px 0; border: 1px solid #444; background: #2a2a2a; color: #fff; border-radius: 4px; }";
  html += "button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 4px; font-size: 16px; cursor: pointer; }";
  html += "button:hover { background: #45a049; }";
  html += ".info { background: #2a2a2a; padding: 15px; border-radius: 4px; margin: 20px 0; }";
  html += "</style></head><body>";
  html += "<h1>Silver Tracka WiFi Setup</h1>";
  html += "<div class='info'>";
  html += "<p>Enter your WiFi network credentials below:</p>";
  html += "</div>";
  html += "<form action='/configure' method='POST'>";
  html += "<label>WiFi Network Name (SSID):</label>";
  html += "<input type='text' name='ssid' required placeholder='Your WiFi Name'>";
  html += "<label>WiFi Password:</label>";
  html += "<input type='password' name='password' required placeholder='Your WiFi Password'>";
  html += "<button type='submit'>Connect</button>";
  html += "</form>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigure() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    
    // Save credentials
    preferences.putString("ssid", newSSID);
    preferences.putString("password", newPassword);
    
    // Update global variables
    ssid = newSSID;
    password = newPassword;
    
    // Send success response
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Configuration Saved</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; background: #1a1a1a; color: #fff; padding: 20px; text-align: center; }";
    html += "h1 { color: #4CAF50; }";
    html += ".info { background: #2a2a2a; padding: 20px; border-radius: 4px; margin: 20px auto; max-width: 400px; }";
    html += "</style></head><body>";
    html += "<h1>Configuration Saved!</h1>";
    html += "<div class='info'>";
    html += "<p>WiFi credentials saved. The device will now attempt to connect.</p>";
    html += "<p>Please wait while the device restarts...</p>";
    html += "</div>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    delay(1000);
    
    // Stop AP and web server
    server.stop();
    WiFi.softAPdisconnect(true);
    
    // Try to connect to the new WiFi
    Serial.println("Attempting to connect to new WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to WiFi!");
      wifiConfigured = true;
      apMode = false;
      
      // Set timezone
      configTime(0, 0, "pool.ntp.org");
      
      // Show success on display
      tft.fillScreen(TFT_BLACK);
      drawText(160, 120, "Connected!", FONT_MEDIUM, true);
      
      // Set wake time for deep sleep countdown
      wakeTime = millis();
      shouldSleep = true;
    } else {
      Serial.println("\nFailed to connect - clearing credentials and restarting AP mode...");
      clearWiFiCredentials();
      delay(2000);
      ESP.restart();
    }
  } else {
    server.send(400, "text/plain", "Missing SSID or password");
  }
}

