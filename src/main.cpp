#include <Arduino.h>
#include <heltec-eink-modules.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeMono9pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

// First, create an adapter that converts your display to Adafruit_GFX
class EInkGFXAdapter : public Adafruit_GFX {
private:
    EInkDisplay_VisionMasterE290& display;

public:
    EInkGFXAdapter(EInkDisplay_VisionMasterE290& d)
        : Adafruit_GFX(d.width(), d.height()), display(d) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        // U8G2 typically uses 1 for black and 0 for white
        // Convert this to your e-ink display's color scheme
        if (color == 1) {
            display.drawPixel(x, y, BLACK);  // or whatever constant your display uses for black
        } else {
            display.drawPixel(x, y, WHITE);  // or whatever constant your display uses for white
        }
    }
};

// Then use this adapter with U8g2
class EInkU8g2Adapter : public U8G2_FOR_ADAFRUIT_GFX {
private:
    EInkGFXAdapter gfx_adapter;

public:
    EInkU8g2Adapter(EInkDisplay_VisionMasterE290& d)
        : gfx_adapter(d) {}

    void begin() {
        U8G2_FOR_ADAFRUIT_GFX::begin(gfx_adapter);
    }
};

EInkDisplay_VisionMasterE290 display;
EInkU8g2Adapter u8g2_adapter(display);

// WiFi and API configuration
const char* API_BASE_URL = "https://data.etabus.gov.hk";
const char* DEFAULT_ROUTE_NUMBER = "299X";
const char* STOP_ID = "ST142";  // Fallback stop_id if dynamic stop selection fails
const char* DEFAULT_BOUND = "O";  // O for outbound, I for inbound
const int DEFAULT_SERVICE_TYPE = 1;
const int DEFAULT_STOP_SEQ = 3;   // Select the 3rd stop from route-stop list

#ifndef SETUP_BUTTON_PIN
// Defaults to the BOOT key on ESP32 boards. If your enclosure has a separate
// setup/reset button, override this in platformio.ini with -D SETUP_BUTTON_PIN=<gpio>.
#define SETUP_BUTTON_PIN 0
#endif

const int SETUP_BUTTON_ACTIVE_LEVEL = LOW;
const unsigned long SETUP_HOLD_WINDOW_MS = 1800;
const unsigned long CONFIG_PORTAL_TIMEOUT_SECONDS = 600; // 10 minutes
const unsigned long OTA_WINDOW_SECONDS = 180; // OTA is offered after setup/button wake
const int DEFAULT_SLEEP_MINUTES = 5;
const int DEFAULT_ALERT_MINUTES = 5;
const int MIN_SLEEP_MINUTES = 1;
const int MAX_SLEEP_MINUTES = 60;
const int MAX_ROUTE_STOPS = 100;
const int MAX_CONFIGURED_ROUTES = 3;
const int MAX_ETAS = 3;
const int MAX_ROUTE_VARIANTS = 12;
const int MAX_ROUTE_SUGGESTIONS = 5;
const int MAX_ROUTE_SEARCH_CHOICES = 36;
const uint64_t US_PER_SECOND = 1000000ULL;
uint32_t plannedSleepSeconds = DEFAULT_SLEEP_MINUTES * 60;
bool portalShouldSaveConfig = false;
bool otaStarted = false;
String otaHostname;

struct RouteConfig {
    String route;
    String bound;
    int service_type;
    int stop_seq;
    String stop_id;
    String stop_name;
};

struct AppConfig {
    RouteConfig routes[MAX_CONFIGURED_ROUTES];
    String route;
    String bound;
    int service_type;
    int stop_seq;
    String stop_id;
    String stop_name;
    int sleep_minutes;
    int alert_minutes;
    bool configured;
};

struct StopCandidate {
    int seq;
    String stop_id;
};

struct RouteVariant {
    String route;
    String bound;
    int service_type;
    String origin_en;
    String origin_tc;
    String dest_en;
    String dest_tc;
};

AppConfig appConfig;
Preferences preferences;
WebServer busSetupServer(80);
bool busSetupSaved = false;
bool busSetupServerStarted = false;

// Structure to hold route information
struct RouteInfo {
    String company;
    String route;
    String bound;
    int service_type;
    String origin_en;
    String origin_tc;
    String dest_en;
    String dest_tc;
    bool valid;
};

// Structure to hold stop (station) information
struct StopInfo {
    String stop_id;
    String name_en;
    String name_tc;
    float lat;
    float lon;
    bool valid;
};

// Structure to hold ETA information
struct ETAInfo {
    String eta;           // ISO 8601 timestamp
    String remark_tc;     // Remark in Traditional Chinese
    String remark_en;     // Remark in English
    String dest_tc;       // Destination in Traditional Chinese
    String dest_en;       // Destination in English
    bool valid;
};

enum RouteFetchStatus {
    ROUTE_STATUS_UNUSED,
    ROUTE_STATUS_OK,
    ROUTE_STATUS_NO_SERVICE,
    ROUTE_STATUS_ERROR
};

struct RouteDisplayState {
    bool enabled;
    RouteFetchStatus status;
    String route;
    String bound;
    int service_type;
    int stop_seq;
    String stop_id;
    String station_name;
    String dest_en;
    String error_text;
    ETAInfo etas[MAX_ETAS];
    int eta_count;
    unsigned long fetch_time;
};

// Array to hold multiple ETAs (next 3 buses)
ETAInfo etaList[MAX_ETAS];
int etaCount = 0;

RouteDisplayState routeStates[MAX_CONFIGURED_ROUTES];
int routeStateCount = 0;
int activeRouteIndex = 0;
bool lastETARequestApiOk = false;
bool lastETARequestNoService = false;

RouteInfo currentRouteInfo;
StopInfo currentStopInfo;
int selectedStopSeq = DEFAULT_STOP_SEQ;
String selectedStopId = String(STOP_ID);
unsigned long lastETAFetchTime = 0; // Track when ETA was last fetched

// Helper function to convert direction code to API format
String convertDirectionToAPI(const char* bound) {
    if (strcmp(bound, "I") == 0 || strcmp(bound, "i") == 0) {
        return "inbound";
    } else if (strcmp(bound, "O") == 0 || strcmp(bound, "o") == 0) {
        return "outbound";
    }
    // If already in correct format, return as-is
    return String(bound);
}

String normalizeBound(String bound) {
    bound.trim();
    bound.toUpperCase();
    if (bound == "INBOUND") return "I";
    if (bound == "OUTBOUND") return "O";
    if (bound == "I" || bound == "O") return bound;
    return String(DEFAULT_BOUND);
}

int clampWithDefault(int value, int minValue, int maxValue, int fallback) {
    if (value < minValue || value > maxValue) return fallback;
    return value;
}

String prefKey(const char* base, int routeIndex) {
    if (routeIndex == 0) return String(base);
    return String(base) + String(routeIndex + 1);
}

String prefStringOrDefault(const String& key, const char* fallback) {
    return preferences.isKey(key.c_str()) ? preferences.getString(key.c_str(), fallback) : String(fallback);
}

void sanitizeRouteConfig(RouteConfig& config, bool required) {
    config.route.trim();
    config.route.toUpperCase();
    if (config.route.length() == 0) {
        if (required) {
            config.route = DEFAULT_ROUTE_NUMBER;
        }
    } else if (config.route.length() > 8) {
        config.route = required ? String(DEFAULT_ROUTE_NUMBER) : "";
    }

    config.bound = normalizeBound(config.bound);
    config.service_type = clampWithDefault(config.service_type, 1, 99, DEFAULT_SERVICE_TYPE);
    config.stop_seq = clampWithDefault(config.stop_seq, 1, 120, DEFAULT_STOP_SEQ);
    config.stop_id.trim();
    config.stop_id.toUpperCase();
    config.stop_name.trim();
}

bool isRouteEnabled(int routeIndex) {
    if (routeIndex < 0 || routeIndex >= MAX_CONFIGURED_ROUTES) return false;
    return appConfig.routes[routeIndex].route.length() > 0;
}

int configuredRouteCount() {
    int count = 0;
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        if (isRouteEnabled(i)) count++;
    }
    return count;
}

String configuredRoutesLabel() {
    String label;
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        if (!isRouteEnabled(i)) continue;
        if (label.length() > 0) label += "/";
        label += appConfig.routes[i].route;
    }
    if (label.length() == 0) label = DEFAULT_ROUTE_NUMBER;
    return label;
}

void applyRouteConfigToActive(int routeIndex) {
    if (routeIndex < 0 || routeIndex >= MAX_CONFIGURED_ROUTES) routeIndex = 0;
    activeRouteIndex = routeIndex;
    appConfig.route = appConfig.routes[routeIndex].route;
    appConfig.bound = appConfig.routes[routeIndex].bound;
    appConfig.service_type = appConfig.routes[routeIndex].service_type;
    appConfig.stop_seq = appConfig.routes[routeIndex].stop_seq;
    appConfig.stop_id = appConfig.routes[routeIndex].stop_id;
    appConfig.stop_name = appConfig.routes[routeIndex].stop_name;
}

void captureActiveRouteConfig(int routeIndex) {
    if (routeIndex < 0 || routeIndex >= MAX_CONFIGURED_ROUTES) return;
    appConfig.routes[routeIndex].route = appConfig.route;
    appConfig.routes[routeIndex].bound = appConfig.bound;
    appConfig.routes[routeIndex].service_type = appConfig.service_type;
    appConfig.routes[routeIndex].stop_seq = appConfig.stop_seq;
    appConfig.routes[routeIndex].stop_id = appConfig.stop_id;
    appConfig.routes[routeIndex].stop_name = appConfig.stop_name;
    sanitizeRouteConfig(appConfig.routes[routeIndex], routeIndex == 0);
}

void sanitizeAppConfig() {
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        sanitizeRouteConfig(appConfig.routes[i], i == 0);
    }
    appConfig.stop_name.trim();
    appConfig.sleep_minutes = clampWithDefault(appConfig.sleep_minutes, MIN_SLEEP_MINUTES, MAX_SLEEP_MINUTES, DEFAULT_SLEEP_MINUTES);
    appConfig.alert_minutes = clampWithDefault(appConfig.alert_minutes, 1, 30, DEFAULT_ALERT_MINUTES);
}

void loadAppConfig() {
    preferences.begin("bus-tag", true);
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        String routeKey = prefKey("route", i);
        String boundKey = prefKey("bound", i);
        String serviceKey = prefKey("service", i);
        String stopSeqKey = prefKey("stopseq", i);
        String stopIdKey = prefKey("stopid", i);
        String stopNameKey = prefKey("stopname", i);

        appConfig.routes[i].route = prefStringOrDefault(routeKey, i == 0 ? DEFAULT_ROUTE_NUMBER : "");
        appConfig.routes[i].bound = prefStringOrDefault(boundKey, DEFAULT_BOUND);
        appConfig.routes[i].service_type = preferences.getInt(serviceKey.c_str(), DEFAULT_SERVICE_TYPE);
        appConfig.routes[i].stop_seq = preferences.getInt(stopSeqKey.c_str(), DEFAULT_STOP_SEQ);
        appConfig.routes[i].stop_id = prefStringOrDefault(stopIdKey, "");
        appConfig.routes[i].stop_name = prefStringOrDefault(stopNameKey, "");
    }
    appConfig.stop_name = appConfig.routes[0].stop_name;
    appConfig.sleep_minutes = preferences.getInt("sleepmin", DEFAULT_SLEEP_MINUTES);
    appConfig.alert_minutes = preferences.getInt("alertmin", DEFAULT_ALERT_MINUTES);
    appConfig.configured = preferences.getBool("configured", false);
    preferences.end();
    sanitizeAppConfig();
    applyRouteConfigToActive(0);
}

void saveAppConfig() {
    sanitizeAppConfig();
    preferences.begin("bus-tag", false);
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        String routeKey = prefKey("route", i);
        String boundKey = prefKey("bound", i);
        String serviceKey = prefKey("service", i);
        String stopSeqKey = prefKey("stopseq", i);
        String stopIdKey = prefKey("stopid", i);
        String stopNameKey = prefKey("stopname", i);

        preferences.putString(routeKey.c_str(), appConfig.routes[i].route);
        preferences.putString(boundKey.c_str(), appConfig.routes[i].bound);
        preferences.putInt(serviceKey.c_str(), appConfig.routes[i].service_type);
        preferences.putInt(stopSeqKey.c_str(), appConfig.routes[i].stop_seq);
        preferences.putString(stopIdKey.c_str(), appConfig.routes[i].stop_id);
        preferences.putString(stopNameKey.c_str(), appConfig.routes[i].stop_name);
    }
    preferences.putInt("sleepmin", appConfig.sleep_minutes);
    preferences.putInt("alertmin", appConfig.alert_minutes);
    preferences.putBool("configured", true);
    preferences.end();
    appConfig.configured = true;
}

bool isSetupButtonPressed() {
    return digitalRead(SETUP_BUTTON_PIN) == SETUP_BUTTON_ACTIVE_LEVEL;
}

void markPortalConfigChanged() {
    portalShouldSaveConfig = true;
}

String clipStatusLine(String value) {
    const int maxChars = 32;
    if (value.length() <= maxChars) return value;
    return value.substring(0, maxChars - 3) + "...";
}

void showStatusMessage(const String& line1, const String& line2, const String& line3) {
    display.clearMemory();
    u8g2_adapter.setFont(u8g2_font_helvR08_tf);
    u8g2_adapter.setCursor(5, 20);
    u8g2_adapter.print(clipStatusLine(line1));
    if (line2.length() > 0) {
        u8g2_adapter.setCursor(5, 36);
        u8g2_adapter.print(clipStatusLine(line2));
    }
    if (line3.length() > 0) {
        u8g2_adapter.setCursor(5, 52);
        u8g2_adapter.print(clipStatusLine(line3));
    }
    display.update();
}

void resetETARequestStatus() {
    lastETARequestApiOk = false;
    lastETARequestNoService = false;
}

bool shouldOpenConfigPortal() {
    if (!appConfig.configured) {
        Serial.println("Bus config missing; opening setup portal");
        return true;
    }

    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT0 || wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println("Setup button woke device; opening setup portal");
        return true;
    }

    esp_reset_reason_t resetReason = esp_reset_reason();
    bool resetStarted = resetReason == ESP_RST_POWERON || resetReason == ESP_RST_EXT || resetReason == ESP_RST_SW;
    if (!resetStarted) return false;

    unsigned long windowStart = millis();
    unsigned long pressedSince = 0;
    while (millis() - windowStart < SETUP_HOLD_WINDOW_MS) {
        if (isSetupButtonPressed()) {
            if (pressedSince == 0) pressedSince = millis();
            if (millis() - pressedSince > 700) {
                Serial.println("Setup button held during reset window; opening setup portal");
                return true;
            }
        } else {
            pressedSince = 0;
        }
        delay(20);
    }

    return false;
}

bool connectWiFiAndMaybeConfigure(bool forcePortal) {
    portalShouldSaveConfig = false;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    WiFiManager wifiManager;
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    wifiManager.setConnectTimeout(20);
    wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SECONDS);
    wifiManager.setSaveConfigCallback(markPortalConfigChanged);

    const char* apName = "Bus-ETA-Setup";
    if (forcePortal || !appConfig.configured) {
        showStatusMessage("WiFi setup", String("AP if needed: ") + apName, "Bus setup after WiFi");
    } else {
        showStatusMessage("Connecting WiFi", configuredRoutesLabel(), "");
    }

    bool connected = wifiManager.autoConnect(apName);
    if (!connected) {
        Serial.println("WiFi setup/connect timed out");
        return false;
    }

    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
}

void beginOTA() {
    if (otaStarted) return;

    otaHostname = "bus-eta-" + appConfig.route;
    otaHostname.toLowerCase();
    ArduinoOTA.setHostname(otaHostname.c_str());

#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

    ArduinoOTA.onStart([]() {
        Serial.println("OTA update started");
        showStatusMessage("OTA updating", otaHostname, "Keep power on");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA update complete");
        showStatusMessage("OTA complete", "Restarting...", "");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        unsigned int percent = total > 0 ? (progress * 100U) / total : 0;
        Serial.printf("OTA progress: %u%%\r", percent);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error[%u]\n", error);
        showStatusMessage("OTA error", String((int)error), "Retry setup mode");
    });

    ArduinoOTA.begin();
    otaStarted = true;

    Serial.print("OTA ready: ");
    Serial.print(otaHostname);
    Serial.print(" at ");
    Serial.println(WiFi.localIP());
}

void runOTAWindow(uint32_t seconds) {
    if (seconds == 0 || WiFi.status() != WL_CONNECTED) return;

    beginOTA();
    showStatusMessage("Saved / OTA ready", WiFi.localIP().toString(), String(seconds / 60) + " min update window");

    unsigned long start = millis();
    while (millis() - start < seconds * 1000UL) {
        ArduinoOTA.handle();
        delay(20);
    }
}

// Forward declaration
bool fetchRouteInfo(const char* route, const char* bound, int service_type);
bool fetchStopInfo(const char* stopId);
bool fetchStopInfoInto(const char* stopId, StopInfo& outInfo);
void displayStopInfoSerial(const StopInfo& info);
bool fetchRouteVariants(const char* route, RouteVariant* variants, int& variantCount, String& suggestions);
bool selectStopIdBySeq(const char* route, const char* bound, int service_type, int seq, String& outStopId);
bool listRouteStops(const char* route, const char* bound, int service_type);
bool resolveStopFromName(const char* route, const char* bound, int service_type);
bool resolveRouteStopFromName(const char* route);
bool fetchRouteETAInfo(const char* route, int service_type, int targetSeq, const char* targetBound);
int calculateMinutesUntilETA(const String& etaStr);
String formatETA(const String& etaStr);
String formatLastUpdated(unsigned long fetchTime);
bool refreshBusData();
uint32_t calculateNextSleepSeconds();
void enterDeepSleep(uint32_t sleepSeconds);
bool runBusSetupServer(uint32_t seconds);

static bool parseRouteVariantObject(const String& objectJson, RouteVariant& variant) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, objectJson);
    if (error || !doc.containsKey("route")) {
        return false;
    }

    variant.route = doc["route"].as<String>();
    variant.route.trim();
    variant.route.toUpperCase();
    variant.bound = normalizeBound(doc["bound"].as<String>());
    String serviceText = doc["service_type"].as<String>();
    variant.service_type = serviceText.toInt();
    if (variant.service_type <= 0) {
        variant.service_type = doc["service_type"].as<int>();
    }
    if (variant.service_type <= 0) {
        variant.service_type = DEFAULT_SERVICE_TYPE;
    }
    variant.origin_en = doc["orig_en"].as<String>();
    variant.origin_tc = doc["orig_tc"].as<String>();
    variant.dest_en = doc["dest_en"].as<String>();
    variant.dest_tc = doc["dest_tc"].as<String>();
    return variant.route.length() > 0;
}

static void appendRouteSuggestion(const RouteVariant& variant, String& suggestions, int& suggestionCount) {
    if (suggestionCount >= MAX_ROUTE_SUGGESTIONS) return;
    if (suggestions.indexOf(variant.route + " ") >= 0) return;

    if (suggestions.length() > 0) suggestions += "; ";
    suggestions += variant.route;
    if (variant.dest_en.length() > 0) {
        suggestions += " to ";
        suggestions += variant.dest_en;
    }
    suggestionCount++;
}

bool fetchRouteVariants(const char* route, RouteVariant* variants, int& variantCount, String& suggestions) {
    variantCount = 0;
    suggestions = "";

    String query = String(route);
    query.trim();
    query.toUpperCase();
    if (query.length() == 0) return false;

    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    String url = String(API_BASE_URL) + "/v1/transport/kmb/route/";
    Serial.println("\n=== Searching KMB Route Table ===");
    Serial.println("Route table URL: " + url);
    Serial.println("Route query: " + query);
    Serial.flush();

    http.begin(client, url);
    http.setTimeout(20000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-Search/1.0");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.print("Route table request failed, code: ");
        Serial.println(httpCode);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    String marker;
    marker.reserve(12);
    String objectJson;
    objectJson.reserve(512);
    bool inDataArray = false;
    bool capturing = false;
    bool inString = false;
    bool escaped = false;
    bool sawExact = false;
    int depth = 0;
    int suggestionCount = 0;
    unsigned long lastByteAt = millis();

    while (http.connected() || stream->available()) {
        if (!stream->available()) {
            if (millis() - lastByteAt > 20000) {
                Serial.println("Timed out while reading route table");
                break;
            }
            delay(1);
            continue;
        }

        char c = (char)stream->read();
        lastByteAt = millis();

        if (!inDataArray) {
            marker += c;
            if (marker.length() > 8) {
                marker.remove(0, marker.length() - 8);
            }
            if (marker.endsWith("\"data\":[")) {
                inDataArray = true;
            }
            continue;
        }

        if (!capturing) {
            if (c == ']') break;
            if (c == '{') {
                capturing = true;
                inString = false;
                escaped = false;
                depth = 1;
                objectJson = "{";
            }
            continue;
        }

        objectJson += c;
        if (escaped) {
            escaped = false;
        } else if (inString && c == '\\') {
            escaped = true;
        } else if (c == '"') {
            inString = !inString;
        } else if (!inString) {
            if (c == '{') depth++;
            if (c == '}') depth--;
        }

        if (objectJson.length() > 768) {
            capturing = false;
            objectJson = "";
            depth = 0;
            continue;
        }

        if (depth == 0) {
            RouteVariant variant;
            if (parseRouteVariantObject(objectJson, variant)) {
                if (variant.route == query) {
                    sawExact = true;
                    if (variantCount < MAX_ROUTE_VARIANTS) {
                        variants[variantCount++] = variant;
                    }
                } else if (sawExact) {
                    break;
                } else if (query.length() > 0 && variant.route.indexOf(query) >= 0) {
                    appendRouteSuggestion(variant, suggestions, suggestionCount);
                }
            }
            capturing = false;
            objectJson = "";
        }
    }

    http.end();

    Serial.print("Route variants found: ");
    Serial.println(variantCount);
    for (int i = 0; i < variantCount; i++) {
        Serial.print("  ");
        Serial.print(variants[i].route);
        Serial.print(" ");
        Serial.print(variants[i].bound);
        Serial.print(" service=");
        Serial.print(variants[i].service_type);
        Serial.print(" ");
        Serial.print(variants[i].origin_en);
        Serial.print(" -> ");
        Serial.println(variants[i].dest_en);
    }
    if (variantCount == 0 && suggestions.length() > 0) {
        Serial.println("Similar routes: " + suggestions);
    }
    Serial.flush();

    return variantCount > 0;
}

// Function to find valid route combinations from Route List API
bool findValidRouteCombinations(const char* route, String& foundBound, int& foundServiceType) {
    RouteVariant variants[MAX_ROUTE_VARIANTS];
    int variantCount = 0;
    String suggestions;
    if (fetchRouteVariants(route, variants, variantCount, suggestions)) {
        foundBound = variants[0].bound;
        foundServiceType = variants[0].service_type;
        return true;
    }
    return false;

    Serial.println("\n=== Querying Route List API to find valid combinations ===");
    Serial.flush();

    WiFiClientSecure client;
    HTTPClient http;

    client.setInsecure();

    String url = String(API_BASE_URL) + "/v1/transport/kmb/route/";

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-Info/1.0");

    Serial.println("URL: " + url);
    Serial.flush();

    int httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    Serial.flush();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.print("Response received, length: ");
        Serial.println(payload.length());

        if (payload.length() == 0) {
            Serial.println("ERROR: Empty response from Route List API");
            Serial.flush();
            http.end();
            return false;
        }

        Serial.print("First 500 chars: ");
        Serial.println(payload.substring(0, 500));
        Serial.flush();

        DynamicJsonDocument doc(65536); // Route list can be very large, increase buffer
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("JSON parsing failed: ");
            Serial.println(error.c_str());
            Serial.print("Error code: ");
            Serial.println(error.code());
            Serial.print("Payload length: ");
            Serial.println(payload.length());
            Serial.flush();
            http.end();
            return false;
        }

        if (doc.containsKey("data") && doc["data"].is<JsonArray>()) {
            JsonArray routes = doc["data"];
            Serial.print("Found ");
            Serial.print(routes.size());
            Serial.println(" total routes in list");
            Serial.print("Searching for route: ");
            Serial.println(route);
            Serial.flush();

            // Find first matching route (exact match)
            for (JsonObject routeData : routes) {
                String routeNum = routeData["route"].as<String>();
                if (routeNum == String(route)) {
                    foundBound = routeData["bound"].as<String>();
                    foundServiceType = routeData["service_type"].as<int>();
                    String orig = routeData["orig_tc"].as<String>();
                    String dest = routeData["dest_tc"].as<String>();

                    Serial.print("Found valid combination: bound=");
                    Serial.print(foundBound);
                    Serial.print(", service_type=");
                    Serial.println(foundServiceType);
                    Serial.print("Route: ");
                    Serial.print(orig);
                    Serial.print(" -> ");
                    Serial.println(dest);
                    Serial.flush();

                    http.end();
                    return true;
                }
            }

            // If exact match not found, search for routes containing the number
            Serial.print("Route ");
            Serial.print(route);
            Serial.println(" not found (exact match)");
            Serial.println("Searching for routes containing '" + String(route) + "'...");
            Serial.flush();

            int matchCount = 0;
            for (JsonObject routeData : routes) {
                String routeNum = routeData["route"].as<String>();
                if (routeNum.indexOf(String(route)) >= 0) {
                    if (matchCount++ < 5) {  // Show first 5 matches
                        Serial.print("  Found similar route: ");
                        Serial.print(routeNum);
                        Serial.print(" (bound=");
                        Serial.print(routeData["bound"].as<String>());
                        Serial.print(", service=");
                        Serial.print(routeData["service_type"].as<int>());
                        Serial.print(") ");
                        Serial.print(routeData["orig_tc"].as<String>());
                        Serial.print(" -> ");
                        Serial.println(routeData["dest_tc"].as<String>());
                    }
                }
            }

            if (matchCount == 0) {
                Serial.println("No routes found containing '" + String(route) + "'");
                Serial.println("Listing first 20 routes in system:");
                int count = 0;
                for (JsonObject routeData : routes) {
                    if (count++ >= 20) break;
                    Serial.print("  Route: ");
                    Serial.print(routeData["route"].as<String>());
                    Serial.print(", Bound: ");
                    Serial.print(routeData["bound"].as<String>());
                    Serial.print(", Service: ");
                    Serial.println(routeData["service_type"].as<int>());
                }
            } else {
                Serial.print("Total matches found: ");
                Serial.println(matchCount);
            }
            Serial.flush();
        } else {
            Serial.println("Response does not contain 'data' array");
            if (doc.containsKey("type")) {
                Serial.print("Response type: ");
                Serial.println(doc["type"].as<String>());
            }
            Serial.flush();
        }

        http.end();
        return false;
    } else if (httpCode < 0) {
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        return false;
    } else {
        Serial.print("Route List API request failed, code: ");
        Serial.println(httpCode);
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        return false;
    }
}

// Function to fetch route information from API with retry logic
bool fetchRouteInfoWithRetry(const char* route) {
    // Try default parameters first
    if (fetchRouteInfo(route, DEFAULT_BOUND, DEFAULT_SERVICE_TYPE)) {
        return true;
    }

    // If default fails, try alternative combinations
    Serial.println("\n=== Retrying with alternative parameters ===");
    Serial.flush();

    // Try inbound with default service type
    if (fetchRouteInfo(route, "I", DEFAULT_SERVICE_TYPE)) {
        return true;
    }

    // Try outbound with service type 2
    if (fetchRouteInfo(route, "O", 2)) {
        return true;
    }

    // Try inbound with service type 2
    if (fetchRouteInfo(route, "I", 2)) {
        return true;
    }

    // If all hardcoded combinations fail, query Route List API to find valid combinations
    Serial.println("\n=== Querying Route List API to discover valid combinations ===");
    Serial.flush();

    String foundBound;
    int foundServiceType;
    if (findValidRouteCombinations(route, foundBound, foundServiceType)) {
        // Convert found bound to direction code for fetchRouteInfo
        const char* boundCode = (foundBound == "I") ? "I" : "O";
        if (fetchRouteInfo(route, boundCode, foundServiceType)) {
            return true;
        }
    }

    Serial.println("All parameter combinations failed");
    Serial.flush();
    return false;
}

// Function to fetch route information from API
bool fetchRouteInfo(const char* route, const char* bound = DEFAULT_BOUND, int service_type = DEFAULT_SERVICE_TYPE) {
    Serial.println("\n=== Starting API Request ===");
    Serial.flush();

    WiFiClientSecure client;
    HTTPClient http;

    // Skip certificate validation for development (not recommended for production)
    client.setInsecure();

    // Convert direction code to API format (inbound/outbound)
    String direction = convertDirectionToAPI(bound);

    // Construct API URL with path parameters
    String url = String(API_BASE_URL) + "/v1/transport/kmb/route/" + String(route) + "/" + direction + "/" + String(service_type);

    Serial.println("URL: " + url);
    Serial.println("Parameters: route=" + String(route) + ", direction=" + direction + ", service_type=" + String(service_type));
    Serial.println("WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("IP: " + WiFi.localIP().toString());
    }
    Serial.flush();

    http.begin(client, url);
    http.setTimeout(15000); // 15 second timeout
    http.addHeader("User-Agent", "ESP32-KMB-Route-Info/1.0");

    Serial.println("Sending GET request...");
    Serial.flush();

    int httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    Serial.flush();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("Response received, length: " + String(payload.length()));
        Serial.println("First 200 chars: " + payload.substring(0, 200));
        Serial.flush();

        // Parse JSON
        DynamicJsonDocument doc(8192);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("JSON parsing failed: ");
            Serial.println(error.c_str());
            Serial.print("Error code: ");
            Serial.println(error.code());
            Serial.flush();
            http.end();
            return false;
        }

        // Extract route information
        // Route API returns a single object, not an array
        if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
            JsonObject routeData = doc["data"];

            // Check if data object is empty (no valid route found)
            if (routeData.size() == 0) {
                Serial.println("Route data object is empty - this route/direction/service_type combination does not exist");
                Serial.flush();
                http.end();
                return false;
            }

            currentRouteInfo.company = routeData["co"].as<String>();
            currentRouteInfo.route = routeData["route"].as<String>();
            currentRouteInfo.bound = routeData["bound"].as<String>();
            currentRouteInfo.service_type = routeData["service_type"].as<int>();
            currentRouteInfo.origin_en = routeData["orig_en"].as<String>();
            currentRouteInfo.origin_tc = routeData["orig_tc"].as<String>();
            currentRouteInfo.dest_en = routeData["dest_en"].as<String>();
            currentRouteInfo.dest_tc = routeData["dest_tc"].as<String>();
            currentRouteInfo.valid = true;

            Serial.println("Route info extracted successfully!");
            Serial.flush();
            http.end();
            return true;
        } else {
            Serial.println("No route data found in response");
            if (doc.containsKey("type")) {
                Serial.println("Response type: " + doc["type"].as<String>());
            }
            Serial.flush();
            http.end();
            return false;
        }
    } else if (httpCode < 0) {
        // Negative codes indicate connection errors
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        return false;
    } else {
        Serial.print("HTTP request failed, code: ");
        Serial.println(httpCode);
        Serial.print("Request parameters used: bound=" + String(bound) + ", service_type=" + String(service_type));
        Serial.println();
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        return false;
    }
}

bool fetchStopInfoInto(const char* stopId, StopInfo& outInfo) {
    Serial.println("\n=== Fetching Stop Information ===");
    Serial.flush();
    outInfo.valid = false;

    WiFiClientSecure client;
    HTTPClient http;

    client.setInsecure();

    String url = String(API_BASE_URL) + "/v1/transport/kmb/stop/" + String(stopId);
    Serial.println("Stop URL: " + url);
    Serial.flush();

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-Stop-Info/1.0");

    int httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    Serial.flush();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.print("Response received, length: ");
        Serial.println(payload.length());
        Serial.flush();

        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            Serial.print("JSON parsing failed: ");
            Serial.println(error.c_str());
            Serial.flush();
            http.end();
            return false;
        }

        if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
            JsonObject data = doc["data"].as<JsonObject>();

            outInfo.stop_id = data["stop"].as<String>();
            outInfo.name_en = data["name_en"].as<String>();
            outInfo.name_tc = data["name_tc"].as<String>();
            outInfo.lat = data["lat"].as<float>();
            outInfo.lon = data["long"].as<float>();
            outInfo.valid = true;

            Serial.println("Successfully loaded stop info");

            http.end();
            return true;
        }

        Serial.println("No stop data found in response");
        if (doc.containsKey("type")) {
            Serial.print("Response type: ");
            Serial.println(doc["type"].as<String>());
        }
        Serial.flush();
        http.end();
        return false;
    } else if (httpCode < 0) {
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        return false;
    } else {
        Serial.print("HTTP request failed, code: ");
        Serial.println(httpCode);
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        return false;
    }
}

// Function to fetch stop (station) information from API
bool fetchStopInfo(const char* stopId) {
    bool ok = fetchStopInfoInto(stopId, currentStopInfo);
    if (ok) {
        displayStopInfoSerial(currentStopInfo);
    } else {
        currentStopInfo.valid = false;
    }
    return ok;
}

void displayStopInfoSerial(const StopInfo& info) {
    Serial.println("\n=== Stop Information ===");
    Serial.println("Selected stop seq: " + String(selectedStopSeq));
    Serial.println("Stop ID: " + (info.valid ? info.stop_id : String(selectedStopId)));
    if (!info.valid) {
        Serial.println("Stop info: invalid/unavailable");
        Serial.println("========================\n");
        return;
    }
    Serial.println("Name (EN): " + info.name_en);
    Serial.println("Name (TC): " + info.name_tc);
    Serial.print("Lat/Lon: ");
    Serial.print(info.lat, 6);
    Serial.print(", ");
    Serial.println(info.lon, 6);
    Serial.println("========================\n");
}

static bool fetchRouteStopListPickSeq(const String& url, const char* boundFilter, int serviceTypeFilter, int seq, String& outStopId) {
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-Stop/1.0");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        return false;
    }

    if (!doc.containsKey("data") || !doc["data"].is<JsonArray>()) {
        return false;
    }

    JsonArray arr = doc["data"].as<JsonArray>();
    for (JsonObject item : arr) {
        if (!item.containsKey("seq") || !item.containsKey("stop")) continue;

        int itemSeq = item["seq"].as<int>();
        if (itemSeq != seq) continue;

        // If the response includes bound/service_type, optionally filter them
        if (boundFilter != nullptr && item.containsKey("bound")) {
            String b = item["bound"].as<String>(); // usually "O"/"I"
            if (b.length() > 0 && b != String(boundFilter)) continue;
        }
        if (item.containsKey("service_type")) {
            int st = item["service_type"].as<int>();
            if (st != serviceTypeFilter) continue;
        }

        outStopId = item["stop"].as<String>();
        return outStopId.length() > 0;
    }

    return false;
}

// Select stop_id by sequence number from route-stop API.
// We try a few known URL shapes for compatibility, then fall back.
bool selectStopIdBySeq(const char* route, const char* bound, int service_type, int seq, String& outStopId) {
    Serial.println("\n=== Selecting Stop by Sequence ===");
    Serial.println("Target: route=" + String(route) + ", bound=" + String(bound) + ", service_type=" + String(service_type) + ", seq=" + String(seq));
    Serial.flush();

    // Try: /route-stop/{route}/{direction}/{service_type}
    String direction = convertDirectionToAPI(bound); // "outbound"/"inbound" if bound is "O"/"I"
    String url1 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + direction + "/" + String(service_type);
    Serial.println("Trying: " + url1);
    if (fetchRouteStopListPickSeq(url1, nullptr, service_type, seq, outStopId)) {
        Serial.println("Selected stop_id: " + outStopId);
        return true;
    }

    // Try: /route-stop/{route}/{bound}/{service_type}
    String url2 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(bound) + "/" + String(service_type);
    Serial.println("Trying: " + url2);
    if (fetchRouteStopListPickSeq(url2, nullptr, service_type, seq, outStopId)) {
        Serial.println("Selected stop_id: " + outStopId);
        return true;
    }

    // Try: /route-stop/{route}/{service_type} then filter by bound if present in response
    String url3 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(service_type);
    Serial.println("Trying: " + url3);
    if (fetchRouteStopListPickSeq(url3, bound, service_type, seq, outStopId)) {
        Serial.println("Selected stop_id: " + outStopId);
        return true;
    }

    Serial.println("Failed to select stop_id by seq; falling back to configured STOP_ID");
    Serial.flush();
    return false;
}

static bool fetchRouteStopCandidatesFromUrl(const String& url, const char* boundFilter, int serviceTypeFilter, StopCandidate* candidates, int& count) {
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-Stop-Resolve/1.0");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.containsKey("data") || !doc["data"].is<JsonArray>()) {
        return false;
    }

    JsonArray arr = doc["data"].as<JsonArray>();
    count = 0;
    for (JsonObject item : arr) {
        if (count >= MAX_ROUTE_STOPS) break;
        if (!item.containsKey("seq") || !item.containsKey("stop")) continue;

        if (boundFilter != nullptr && item.containsKey("bound")) {
            String b = item["bound"].as<String>();
            if (b.length() > 0 && b != String(boundFilter)) continue;
        }
        if (item.containsKey("service_type")) {
            int st = item["service_type"].as<int>();
            if (st != serviceTypeFilter) continue;
        }

        candidates[count].seq = item["seq"].as<int>();
        candidates[count].stop_id = item["stop"].as<String>();
        if (candidates[count].seq > 0 && candidates[count].stop_id.length() > 0) {
            count++;
        }
    }

    return count > 0;
}

static bool fetchRouteStopCandidates(const char* route, const char* bound, int service_type, StopCandidate* candidates, int& count) {
    String direction = convertDirectionToAPI(bound);
    String url1 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + direction + "/" + String(service_type);
    if (fetchRouteStopCandidatesFromUrl(url1, nullptr, service_type, candidates, count)) return true;

    String url2 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(bound) + "/" + String(service_type);
    if (fetchRouteStopCandidatesFromUrl(url2, nullptr, service_type, candidates, count)) return true;

    String url3 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(service_type);
    return fetchRouteStopCandidatesFromUrl(url3, bound, service_type, candidates, count);
}

static String normalizedSearchText(String value) {
    value.trim();
    value.toUpperCase();
    return value;
}

bool resolveStopFromName(const char* route, const char* bound, int service_type) {
    String query = normalizedSearchText(appConfig.stop_name);
    if (query.length() == 0) return false;

    Serial.println("\n=== Resolving Station Name ===");
    Serial.println("Station query: " + appConfig.stop_name);
    Serial.flush();
    showStatusMessage("Resolving station", appConfig.stop_name, String(route) + " " + String(bound));

    StopCandidate candidates[MAX_ROUTE_STOPS];
    int candidateCount = 0;
    if (!fetchRouteStopCandidates(route, bound, service_type, candidates, candidateCount)) {
        Serial.println("Could not load route stops while resolving station name");
        return false;
    }

    int firstContainsIndex = -1;
    StopInfo firstContainsInfo;
    for (int i = 0; i < candidateCount; i++) {
        StopInfo info;
        if (!fetchStopInfoInto(candidates[i].stop_id.c_str(), info)) continue;

        String stopId = normalizedSearchText(candidates[i].stop_id);
        String nameEn = normalizedSearchText(info.name_en);
        String nameTc = normalizedSearchText(info.name_tc);
        bool exactMatch = stopId == query || nameEn == query || nameTc == query;
        bool containsMatch = stopId.indexOf(query) >= 0 || nameEn.indexOf(query) >= 0 || nameTc.indexOf(query) >= 0;

        if (exactMatch || (containsMatch && firstContainsIndex < 0)) {
            firstContainsIndex = i;
            firstContainsInfo = info;
            if (exactMatch) break;
        }
    }

    if (firstContainsIndex < 0 || !firstContainsInfo.valid) {
        Serial.println("No station matched query: " + appConfig.stop_name);
        showStatusMessage("Station not found", appConfig.stop_name, "Check spelling");
        return false;
    }

    selectedStopSeq = candidates[firstContainsIndex].seq;
    selectedStopId = candidates[firstContainsIndex].stop_id;
    currentStopInfo = firstContainsInfo;

    appConfig.stop_seq = selectedStopSeq;
    appConfig.stop_id = selectedStopId;
    if (currentStopInfo.name_en.length() > 0) {
        appConfig.stop_name = currentStopInfo.name_en;
    }
    captureActiveRouteConfig(activeRouteIndex);
    saveAppConfig();

    Serial.print("Resolved station: seq=");
    Serial.print(selectedStopSeq);
    Serial.print(", stop_id=");
    Serial.print(selectedStopId);
    Serial.print(", name=");
    Serial.println(currentStopInfo.name_en);
    Serial.flush();
    return true;
}

bool resolveRouteStopFromName(const char* route) {
    String query = normalizedSearchText(appConfig.stop_name);
    if (query.length() == 0) return false;

    RouteVariant variants[MAX_ROUTE_VARIANTS];
    int variantCount = 0;
    String suggestions;
    showStatusMessage("Checking route", String(route), appConfig.stop_name);

    if (!fetchRouteVariants(route, variants, variantCount, suggestions)) {
        Serial.println("Route not found in KMB route table: " + String(route));
        if (suggestions.length() > 0) {
            Serial.println("Suggestions: " + suggestions);
            showStatusMessage("Route not found", String(route), suggestions);
        } else {
            showStatusMessage("Route not found", String(route), "Check route number");
        }
        return false;
    }

    bool found = false;
    bool foundExact = false;
    RouteVariant matchedVariant;
    StopInfo matchedInfo;
    int matchedSeq = 0;
    String matchedStopId;

    for (int variantIndex = 0; variantIndex < variantCount; variantIndex++) {
        RouteVariant& variant = variants[variantIndex];
        showStatusMessage("Searching station", variant.route + " " + variant.bound, appConfig.stop_name);

        StopCandidate candidates[MAX_ROUTE_STOPS];
        int candidateCount = 0;
        if (!fetchRouteStopCandidates(variant.route.c_str(), variant.bound.c_str(), variant.service_type, candidates, candidateCount)) {
            continue;
        }

        for (int i = 0; i < candidateCount; i++) {
            StopInfo info;
            if (!fetchStopInfoInto(candidates[i].stop_id.c_str(), info)) continue;

            String stopId = normalizedSearchText(candidates[i].stop_id);
            String nameEn = normalizedSearchText(info.name_en);
            String nameTc = normalizedSearchText(info.name_tc);
            bool exactMatch = stopId == query || nameEn == query || nameTc == query;
            bool containsMatch = stopId.indexOf(query) >= 0 || nameEn.indexOf(query) >= 0 || nameTc.indexOf(query) >= 0;

            if (exactMatch || (containsMatch && !found)) {
                found = true;
                foundExact = exactMatch;
                matchedVariant = variant;
                matchedInfo = info;
                matchedSeq = candidates[i].seq;
                matchedStopId = candidates[i].stop_id;
                if (exactMatch) break;
            }
        }

        if (foundExact) break;
    }

    if (!found || !matchedInfo.valid) {
        Serial.println("Station not found on route: " + appConfig.stop_name + " route=" + String(route));
        showStatusMessage("Station not found", appConfig.stop_name, String(route));
        return false;
    }

    appConfig.route = matchedVariant.route;
    appConfig.bound = matchedVariant.bound;
    appConfig.service_type = matchedVariant.service_type;
    appConfig.stop_seq = matchedSeq;
    appConfig.stop_id = matchedStopId;
    if (matchedInfo.name_en.length() > 0) {
        appConfig.stop_name = matchedInfo.name_en;
    }

    selectedStopSeq = matchedSeq;
    selectedStopId = matchedStopId;
    currentStopInfo = matchedInfo;
    currentRouteInfo.company = "KMB";
    currentRouteInfo.route = matchedVariant.route;
    currentRouteInfo.bound = matchedVariant.bound;
    currentRouteInfo.service_type = matchedVariant.service_type;
    currentRouteInfo.origin_en = matchedVariant.origin_en;
    currentRouteInfo.origin_tc = matchedVariant.origin_tc;
    currentRouteInfo.dest_en = matchedVariant.dest_en;
    currentRouteInfo.dest_tc = matchedVariant.dest_tc;
    currentRouteInfo.valid = true;

    captureActiveRouteConfig(activeRouteIndex);
    saveAppConfig();

    Serial.print("Resolved route/stop: route=");
    Serial.print(appConfig.route);
    Serial.print(", bound=");
    Serial.print(appConfig.bound);
    Serial.print(", service=");
    Serial.print(appConfig.service_type);
    Serial.print(", seq=");
    Serial.print(selectedStopSeq);
    Serial.print(", stop_id=");
    Serial.print(selectedStopId);
    Serial.print(", name=");
    Serial.println(currentStopInfo.name_en);
    Serial.flush();
    return true;
}

// List all stops for a route, displaying sequence, stop_id, and stop name
// Optimized to avoid stack overflow by processing items one at a time
bool listRouteStops(const char* route, const char* bound, int service_type) {
    Serial.println("\n=== Listing Route Stops ===");
    Serial.print("Route: ");
    Serial.print(route);
    Serial.print(", Bound: ");
    Serial.print(bound);
    Serial.print(", Service Type: ");
    Serial.println(service_type);
    Serial.flush();

    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    // Try the same URL patterns as selectStopIdBySeq
    String direction = convertDirectionToAPI(bound);
    String url1 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + direction + "/" + String(service_type);
    String url2 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(bound) + "/" + String(service_type);
    String url3 = String(API_BASE_URL) + "/v1/transport/kmb/route-stop/" + String(route) + "/" + String(service_type);

    const char* urls[] = {url1.c_str(), url2.c_str(), url3.c_str()};

    for (int urlIdx = 0; urlIdx < 3; urlIdx++) {
        Serial.print("Trying URL: ");
        Serial.println(urls[urlIdx]);
        Serial.flush();

        http.begin(client, urls[urlIdx]);
        http.setTimeout(15000);
        http.addHeader("User-Agent", "ESP32-KMB-Route-Stop-List/1.0");

        int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            Serial.print("HTTP Code: ");
            Serial.println(httpCode);
            http.end();
            continue;
        }

        String payload = http.getString();
        http.end();

        // Use smaller buffer and process incrementally
        DynamicJsonDocument doc(16384); // Reduced from 32768
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            Serial.print("JSON parse error: ");
            Serial.println(error.c_str());
            continue;
        }

        if (!doc.containsKey("data") || !doc["data"].is<JsonArray>()) {
            Serial.println("Response does not contain 'data' array");
            continue;
        }

        JsonArray arr = doc["data"].as<JsonArray>();
        Serial.println("\n--- Route Stop List ---");
        Serial.print("Total stops found: ");
        Serial.println(arr.size());
        Serial.println("Seq | Stop ID        | Bound | Service");
        Serial.println("----+----------------+-------+---------");

        // Process and display stops one at a time to avoid stack overflow
        // First pass: count and collect minimal data (just seq numbers for sorting)
        int seqArray[100]; // Max 100 stops to avoid stack overflow
        int stopIdIndices[100];
        int entryCount = 0;

        for (JsonObject item : arr) {
            if (entryCount >= 100) break;

            // Filter by bound if specified and present in response
            if (bound != nullptr && item.containsKey("bound")) {
                const char* itemBound = item["bound"].as<const char*>();
                if (itemBound != nullptr && strlen(itemBound) > 0 && strcmp(itemBound, bound) != 0) {
                    continue;
                }
            }

            // Filter by service_type if present in response
            if (item.containsKey("service_type")) {
                int st = item["service_type"].as<int>();
                if (st != service_type) {
                    continue;
                }
            }

            if (item.containsKey("seq") && item.containsKey("stop")) {
                seqArray[entryCount] = item["seq"].as<int>();
                stopIdIndices[entryCount] = entryCount;
                entryCount++;
            }
        }

        // Simple sort of indices by sequence
        for (int i = 0; i < entryCount - 1; i++) {
            for (int j = 0; j < entryCount - i - 1; j++) {
                if (seqArray[stopIdIndices[j]] > seqArray[stopIdIndices[j + 1]]) {
                    int temp = stopIdIndices[j];
                    stopIdIndices[j] = stopIdIndices[j + 1];
                    stopIdIndices[j + 1] = temp;
                }
            }
        }

        // Second pass: display in sorted order (re-iterate to get stop_id)
        int displayed = 0;
        for (int sortedIdx = 0; sortedIdx < entryCount; sortedIdx++) {
            int targetSeq = seqArray[stopIdIndices[sortedIdx]];

            // Find the item with this sequence
            for (JsonObject item : arr) {
                if (!item.containsKey("seq") || !item.containsKey("stop")) continue;
                if (item["seq"].as<int>() != targetSeq) continue;

                // Filter checks again
                if (bound != nullptr && item.containsKey("bound")) {
                    const char* itemBound = item["bound"].as<const char*>();
                    if (itemBound != nullptr && strlen(itemBound) > 0 && strcmp(itemBound, bound) != 0) {
                        continue;
                    }
                }
                if (item.containsKey("service_type")) {
                    int st = item["service_type"].as<int>();
                    if (st != service_type) continue;
                }

                // Display this stop
                int seq = item["seq"].as<int>();
                const char* stopId = item["stop"].as<const char*>();
                const char* itemBound = item.containsKey("bound") ? item["bound"].as<const char*>() : "-";
                int st = item.containsKey("service_type") ? item["service_type"].as<int>() : service_type;

                Serial.print(seq);
                Serial.print("   | ");
                Serial.print(stopId != nullptr ? stopId : "N/A");
                Serial.print(" | ");
                Serial.print(itemBound != nullptr ? itemBound : "-");
                Serial.print("     | ");
                Serial.println(st);

                displayed++;
                break; // Found and displayed, move to next
            }
        }

        Serial.println("----+----------------+-------+---------");
        Serial.print("Displayed ");
        Serial.print(displayed);
        Serial.println(" stops");
        Serial.println("Note: Use stop_id to fetch ETA. Stop names can be fetched separately if needed.");
        Serial.flush();
        return true;
    }

    Serial.println("Failed to fetch route-stop list from any URL pattern");
    Serial.flush();
    return false;
}

// Function to fetch ETA information using Route ETA API (recommended method)
// This API returns ETAs for all stops on the route, which we filter by seq and dir
bool fetchRouteETAInfo(const char* route, int service_type, int targetSeq, const char* targetBound) {
    Serial.println("\n=== Fetching ETA Information (Route ETA API) ===");
    Serial.flush();
    resetETARequestStatus();

    WiFiClientSecure client;
    HTTPClient http;

    client.setInsecure();

    // Construct Route ETA API URL: /v1/transport/kmb/route-eta/{route}/{service_type}
    String url = String(API_BASE_URL) + "/v1/transport/kmb/route-eta/" + String(route) + "/" + String(service_type);

    Serial.println("Route ETA URL: " + url);
    Serial.print("Filtering by: seq=");
    Serial.print(targetSeq);
    Serial.print(", dir=");
    Serial.println(targetBound);
    Serial.flush();

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-ETA/1.0");

    int httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    Serial.flush();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.print("Response received, length: ");
        Serial.println(payload.length());
        Serial.flush();

        // Increase buffer size to handle large responses (service_type 1 can be 40KB+)
        // Use 60KB buffer to provide headroom
        DynamicJsonDocument doc(61440); // 60KB buffer for large Route ETA responses
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("JSON parsing failed: ");
            Serial.println(error.c_str());
            Serial.print("Error code: ");
            Serial.println(error.code());
            Serial.print("Payload length: ");
            Serial.println(payload.length());
            Serial.print("Buffer size: 61440 bytes");
            if (payload.length() > 61440) {
                Serial.print(" (WARNING: Payload larger than buffer!)");
            }
            Serial.println();
            Serial.flush();
            http.end();
            return false;
        }

        // Route ETA API returns an array of ETA objects for all stops
        if (doc.containsKey("data") && doc["data"].is<JsonArray>()) {
            lastETARequestApiOk = true;
            lastETAFetchTime = millis();
            JsonArray etas = doc["data"];
            Serial.print("Found ");
            Serial.print(etas.size());
            Serial.println(" total ETA entries (all stops)");
            Serial.flush();

            // Diagnostic: Show sample seq/dir values from first few entries
            Serial.println("\n=== Diagnostic: Sample entries from response ===");
            int sampleCount = 0;
            int uniqueSeqs[50] = {0};
            int uniqueSeqCount = 0;
            String uniqueDirs[10];
            int uniqueDirCount = 0;

            for (JsonObject etaData : etas) {
                if (sampleCount < 10) {
                    int seq = etaData.containsKey("seq") ? etaData["seq"].as<int>() : -1;
                    String dir = etaData.containsKey("dir") ? etaData["dir"].as<String>() : "?";
                    int eta_seq = etaData.containsKey("eta_seq") ? etaData["eta_seq"].as<int>() : -1;
                    String route = etaData.containsKey("route") ? etaData["route"].as<String>() : "?";
                    int st = etaData.containsKey("service_type") ? etaData["service_type"].as<int>() : -1;

                    Serial.print("  Entry #");
                    Serial.print(sampleCount);
                    Serial.print(": route=");
                    Serial.print(route);
                    Serial.print(", seq=");
                    Serial.print(seq);
                    Serial.print(", dir=");
                    Serial.print(dir);
                    Serial.print(", service_type=");
                    Serial.print(st);
                    Serial.print(", eta_seq=");
                    Serial.println(eta_seq);

                    // Track unique seq values
                    bool seqFound = false;
                    for (int i = 0; i < uniqueSeqCount; i++) {
                        if (uniqueSeqs[i] == seq) {
                            seqFound = true;
                            break;
                        }
                    }
                    if (!seqFound && seq > 0 && uniqueSeqCount < 50) {
                        uniqueSeqs[uniqueSeqCount++] = seq;
                    }

                    // Track unique dir values
                    bool dirFound = false;
                    for (int i = 0; i < uniqueDirCount; i++) {
                        if (uniqueDirs[i] == dir) {
                            dirFound = true;
                            break;
                        }
                    }
                    if (!dirFound && dir != "?" && uniqueDirCount < 10) {
                        uniqueDirs[uniqueDirCount++] = dir;
                    }
                }
                sampleCount++;
                if (sampleCount >= 10) break;
            }

            Serial.print("\nUnique seq values found (first 20): ");
            for (int i = 0; i < uniqueSeqCount && i < 20; i++) {
                Serial.print(uniqueSeqs[i]);
                if (i < uniqueSeqCount - 1 && i < 19) Serial.print(", ");
            }
            Serial.println();

            Serial.print("Unique dir values found: ");
            for (int i = 0; i < uniqueDirCount; i++) {
                Serial.print(uniqueDirs[i]);
                if (i < uniqueDirCount - 1) Serial.print(", ");
            }
            Serial.println();

            Serial.print("Target filters: seq=");
            Serial.print(targetSeq);
            Serial.print(", dir=");
            Serial.println(targetBound);
            Serial.println("=== End Diagnostic ===\n");
            Serial.flush();

            // Clear previous ETAs
            etaCount = 0;
            for (int i = 0; i < MAX_ETAS; i++) {
                etaList[i].valid = false;
            }

            // Filter by seq and dir, then sort by eta_seq
            // First, collect matching ETAs with their eta_seq for sorting
            struct TempETA {
                String eta;
                String remark_tc;
                String remark_en;
                String dest_tc;
                String dest_en;
                int eta_seq;
            };
            TempETA tempEtas[20]; // Max 20 ETAs to sort
            int tempCount = 0;

            // Filtering statistics
            int totalEntries = 0;
            int withSeq = 0;
            int matchingSeq = 0;
            int withDir = 0;
            int matchingDir = 0;
            int withEta = 0;

            for (JsonObject etaData : etas) {
                totalEntries++;

                // Filter by sequence
                if (!etaData.containsKey("seq")) continue;
                withSeq++;
                // Handle seq as either int or string (some APIs might return string)
                int seq = -1;
                if (etaData["seq"].is<int>()) {
                    seq = etaData["seq"].as<int>();
                } else if (etaData["seq"].is<String>()) {
                    seq = etaData["seq"].as<String>().toInt();
                }
                if (seq != targetSeq) continue;
                matchingSeq++;

                // Filter by direction
                if (!etaData.containsKey("dir")) continue;
                withDir++;
                String dir = etaData["dir"].as<String>();
                // Normalize direction comparison (trim whitespace, case-sensitive)
                dir.trim();
                String targetDir = String(targetBound);
                targetDir.trim();
                // Explicit comparison with debug info for first mismatch
                if (dir != targetDir) {
                    // Log first mismatch for debugging
                    if (matchingSeq == 1 && matchingDir == 0) {
                        Serial.print("DEBUG: First dir mismatch - dir='");
                        Serial.print(dir);
                        Serial.print("' (len=");
                        Serial.print(dir.length());
                        Serial.print("), target='");
                        Serial.print(targetDir);
                        Serial.print("' (len=");
                        Serial.print(targetDir.length());
                        Serial.println(")");
                        Serial.flush();
                    }
                    continue;
                }
                matchingDir++;

                // Check for valid ETA
                if (!etaData.containsKey("eta")) continue;
                String etaStr = etaData["eta"].as<String>();
                if (etaStr.length() == 0 || etaStr == "null") continue;
                withEta++;

                // Store in temp array for sorting
                if (tempCount < 20) {
                    tempEtas[tempCount].eta = etaStr;
                    tempEtas[tempCount].remark_tc = etaData["rmk_tc"].as<String>();
                    tempEtas[tempCount].remark_en = etaData["rmk_en"].as<String>();
                    tempEtas[tempCount].dest_tc = etaData["dest_tc"].as<String>();
                    tempEtas[tempCount].dest_en = etaData["dest_en"].as<String>();
                    tempEtas[tempCount].eta_seq = etaData.containsKey("eta_seq") ? etaData["eta_seq"].as<int>() : tempCount + 1;
                    tempCount++;
                }
            }

            // Log filtering statistics
            Serial.println("=== Filtering Statistics ===");
            Serial.print("  Total entries processed: ");
            Serial.println(totalEntries);
            Serial.print("  Entries with 'seq' field: ");
            Serial.println(withSeq);
            Serial.print("  Entries matching seq=");
            Serial.print(targetSeq);
            Serial.print(": ");
            Serial.println(matchingSeq);
            Serial.print("  Entries with 'dir' field: ");
            Serial.println(withDir);
            Serial.print("  Entries matching dir=");
            Serial.print(targetBound);
            Serial.print(": ");
            Serial.println(matchingDir);
            Serial.print("  Entries with valid 'eta': ");
            Serial.println(withEta);
            Serial.print("  Final ETAs collected: ");
            Serial.println(tempCount);
            Serial.println("=== End Statistics ===\n");
            Serial.flush();

            // Sort by eta_seq (simple bubble sort)
            for (int i = 0; i < tempCount - 1; i++) {
                for (int j = 0; j < tempCount - i - 1; j++) {
                    if (tempEtas[j].eta_seq > tempEtas[j + 1].eta_seq) {
                        TempETA temp = tempEtas[j];
                        tempEtas[j] = tempEtas[j + 1];
                        tempEtas[j + 1] = temp;
                    }
                }
            }

            // Copy sorted ETAs to etaList (up to MAX_ETAS)
            int added = 0;
            for (int i = 0; i < tempCount && added < MAX_ETAS; i++) {
                etaList[added].eta = tempEtas[i].eta;
                etaList[added].remark_tc = tempEtas[i].remark_tc;
                etaList[added].remark_en = tempEtas[i].remark_en;
                etaList[added].dest_tc = tempEtas[i].dest_tc;
                etaList[added].dest_en = tempEtas[i].dest_en;
                etaList[added].valid = true;
                added++;

                Serial.print("  ETA #");
                Serial.print(added);
                Serial.print(" (eta_seq=");
                Serial.print(tempEtas[i].eta_seq);
                Serial.print("): ");
                Serial.print(tempEtas[i].eta);

                // Calculate and display minutes until ETA
                int minutes = calculateMinutesUntilETA(tempEtas[i].eta);
                if (minutes >= 0 && minutes < 10000) {
                    Serial.print(" (");
                    Serial.print(minutes);
                    Serial.print(" min)");
                } else if (minutes < 0 && minutes > -10000) {
                    Serial.print(" (-");
                    Serial.print(-minutes);
                    Serial.print(" min ago)");
                }

                Serial.print(" -> ");
                Serial.println(tempEtas[i].dest_en);
                Serial.flush();
            }

            etaCount = added;

            if (etaCount > 0) {
                Serial.print("Successfully loaded ");
                Serial.print(etaCount);
                Serial.print(" ETAs for seq=");
                Serial.print(targetSeq);
                Serial.print(", dir=");
                Serial.println(targetBound);
            } else {
                lastETARequestNoService = matchingSeq > 0 && matchingDir > 0;
                Serial.print("No ETAs found matching filters: seq=");
                Serial.print(targetSeq);
                Serial.print(", dir=");
                Serial.print(targetBound);
                Serial.print(" (from ");
                Serial.print(etas.size());
                Serial.println(" total entries)");
                Serial.println("  This may indicate:");
                Serial.println("    - No buses scheduled for this stop/direction");
                Serial.println("    - Incorrect seq or direction filter");
                Serial.println("    - Service type mismatch");
            }
            Serial.flush();

            http.end();
            return etaCount > 0;
        } else {
            Serial.println("No ETA data found in Route ETA API response");
            if (doc.containsKey("type")) {
                Serial.print("  Response type: ");
                Serial.println(doc["type"].as<String>());
            }
            Serial.flush();
            http.end();
            return false;
        }
    } else if (httpCode < 0) {
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        return false;
    } else {
        Serial.print("HTTP request failed, code: ");
        Serial.println(httpCode);
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        return false;
    }
}

// Function to fetch ETA information from API (fallback method)
bool fetchETAInfo(const char* route, const char* stop, int service_type) {
    Serial.println("\n=== Fetching ETA Information ===");
    Serial.flush();
    resetETARequestStatus();

    WiFiClientSecure client;
    HTTPClient http;

    client.setInsecure();

    // Construct ETA API URL: /v1/transport/kmb/eta/{route}/{stop}/{service_type}
    String url = String(API_BASE_URL) + "/v1/transport/kmb/eta/" + String(route) + "/" + String(stop) + "/" + String(service_type);

    Serial.println("ETA URL: " + url);
    Serial.flush();

    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "ESP32-KMB-ETA/1.0");

    int httpCode = http.GET();

    Serial.print("HTTP Response Code: ");
    Serial.println(httpCode);
    Serial.flush();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.print("Response received, length: ");
        Serial.println(payload.length());
        Serial.flush();

        // Print response payload for debugging (first 500 chars)
        Serial.println("Response payload (first 500 chars):");
        int printLen = payload.length() > 500 ? 500 : payload.length();
        Serial.println(payload.substring(0, printLen));
        if (payload.length() > 500) {
            Serial.println("... (truncated)");
        }
        Serial.flush();

        DynamicJsonDocument doc(16384);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("JSON parsing failed: ");
            Serial.println(error.c_str());
            Serial.flush();
            http.end();
            return false;
        }

        // Log JSON document structure for debugging
        Serial.println("JSON Document Structure:");
        Serial.print("  Root type: ");
        if (doc.is<JsonObject>()) {
            Serial.println("Object");
            Serial.print("  Root keys: ");
            bool first = true;
            for (JsonPair kv : doc.as<JsonObject>()) {
                if (!first) Serial.print(", ");
                Serial.print(kv.key().c_str());
                Serial.print("(");
                if (kv.value().is<JsonArray>()) Serial.print("Array");
                else if (kv.value().is<JsonObject>()) Serial.print("Object");
                else if (kv.value().is<String>()) Serial.print("String");
                else if (kv.value().is<int>()) Serial.print("int");
                else if (kv.value().is<float>()) Serial.print("float");
                else if (kv.value().is<bool>()) Serial.print("bool");
                else Serial.print("other");
                Serial.print(")");
                first = false;
            }
            Serial.println();
        } else if (doc.is<JsonArray>()) {
            Serial.println("Array");
        } else {
            Serial.println("Unknown");
        }
        Serial.flush();

        // Check for error messages or unexpected fields
        if (doc.containsKey("error") || doc.containsKey("message") || doc.containsKey("warning")) {
            Serial.println("WARNING: Response contains error/warning fields:");
            if (doc.containsKey("error")) {
                Serial.print("  error: ");
                Serial.println(doc["error"].as<String>());
            }
            if (doc.containsKey("message")) {
                Serial.print("  message: ");
                Serial.println(doc["message"].as<String>());
            }
            if (doc.containsKey("warning")) {
                Serial.print("  warning: ");
                Serial.println(doc["warning"].as<String>());
            }
            Serial.flush();
        }

        // ETA API returns an array of ETA objects
        if (doc.containsKey("data") && doc["data"].is<JsonArray>()) {
            lastETARequestApiOk = true;
            lastETAFetchTime = millis();
            JsonArray etas = doc["data"];
            Serial.print("Found ");
            Serial.print(etas.size());
            Serial.println(" ETA entries");
            Serial.flush();

            // Clear previous ETAs
            etaCount = 0;
            for (int i = 0; i < MAX_ETAS; i++) {
                etaList[i].valid = false;
            }

            // Get next MAX_ETAS buses, WITHOUT filtering by direction (temporarily disabled for diagnostics)
            int added = 0;
            String targetBound = String(DEFAULT_BOUND); // "O" for outbound
            int totalWithEta = 0;
            int totalMatchingDir = 0;
            int totalInbound = 0;
            int totalOutbound = 0;

            Serial.println("Processing ETAs (direction filter DISABLED for diagnostics):");
            Serial.flush();

            for (JsonObject etaData : etas) {
                if (added >= MAX_ETAS) break;

                String etaStr = etaData["eta"].as<String>();
                String dir = etaData["dir"].as<String>(); // Direction: "O" or "I"

                // Skip if ETA is empty or null
                if (etaStr.length() == 0 || etaStr == "null") {
                    continue;
                }
                totalWithEta++;

                // Count directions for diagnostics
                if (dir == "O") {
                    totalOutbound++;
                } else if (dir == "I") {
                    totalInbound++;
                }

                // TEMPORARILY DISABLED: Filter by bound - only include outbound (O) buses to Sai Kung
                // if (dir != targetBound) {
                //     continue;
                // }
                totalMatchingDir++;

                etaList[added].eta = etaStr;
                etaList[added].remark_tc = etaData["rmk_tc"].as<String>();
                etaList[added].remark_en = etaData["rmk_en"].as<String>();
                etaList[added].dest_tc = etaData["dest_tc"].as<String>();
                etaList[added].dest_en = etaData["dest_en"].as<String>();
                etaList[added].valid = true;
                added++;

                // Log each ETA found for diagnostics
                Serial.print("  ETA #");
                Serial.print(added);
                Serial.print(": ");
                Serial.print(etaStr);
                Serial.print(" (dir=");
                Serial.print(dir);
                Serial.print(", dest=");
                Serial.print(etaData["dest_en"].as<String>());
                Serial.println(")");
                Serial.flush();
            }

            etaCount = added;

            Serial.print("Successfully loaded ");
            Serial.print(etaCount);
            Serial.println(" ETAs");
            Serial.println("Direction statistics (filter DISABLED):");
            Serial.print("  Total ETAs with valid time: ");
            Serial.println(totalWithEta);
            Serial.print("  Outbound (O): ");
            Serial.println(totalOutbound);
            Serial.print("  Inbound (I): ");
            Serial.println(totalInbound);
            Serial.print("  Target bound was: ");
            Serial.println(targetBound);
            Serial.print("  ETAs added (no filter): ");
            Serial.println(etaCount);
            Serial.flush();

            if (etas.size() == 0) {
                lastETARequestNoService = true;
                Serial.println("Note: ETA data array is empty. This often means stop_id is not served by this route/service_type, or no upcoming buses.");
                Serial.println("Diagnostics:");
                Serial.print("  URL: ");
                Serial.println(url);
                Serial.print("  Route: ");
                Serial.println(route);
                Serial.print("  Stop ID: ");
                Serial.println(stop);
                Serial.print("  Service Type: ");
                Serial.println(service_type);
                Serial.println("  Suggestion: Verify stop_id is correct for this route/service_type combination.");
                Serial.println("  Suggestion: Try calling listRouteStops() to see valid stop_ids for this route.");
                Serial.println("  Suggestion: Try different service_type values (1, 2, etc.) if available.");
            } else if (etaCount == 0) {
                lastETARequestNoService = true;
                Serial.println("WARNING: Array has " + String(etas.size()) + " entries but 0 ETAs were added after processing.");
                Serial.println("This suggests all ETAs were filtered out or had invalid data.");
            } else {
                Serial.println("Note: Direction filter is currently DISABLED - all ETAs are being included.");
            }
            Serial.flush();

            http.end();
            return etaCount > 0;
        } else {
            Serial.println("No ETA data found in response");
            Serial.println("Response structure analysis:");
            if (doc.containsKey("type")) {
                Serial.print("  Response type: ");
                Serial.println(doc["type"].as<String>());
            }
            if (doc.containsKey("data")) {
                Serial.print("  'data' field exists but is not an array. Type: ");
                if (doc["data"].isNull()) Serial.println("null");
                else if (doc["data"].is<JsonObject>()) Serial.println("Object");
                else if (doc["data"].is<String>()) Serial.println("String");
                else if (doc["data"].is<int>()) Serial.println("int");
                else Serial.println("Unknown");
            } else {
                Serial.println("  'data' field is missing from response");
            }
            Serial.println("Diagnostics:");
            Serial.print("  URL: ");
            Serial.println(url);
            Serial.print("  Route: ");
            Serial.println(route);
            Serial.print("  Stop ID: ");
            Serial.println(stop);
            Serial.print("  Service Type: ");
            Serial.println(service_type);
            Serial.println("Hint: verify route/stop_id/service_type are correct. stop_id should be a KMB stop_id from route-stop list.");
            Serial.flush();
            http.end();
            return false;
        }
    } else if (httpCode < 0) {
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        return false;
    } else {
        Serial.print("HTTP request failed, code: ");
        Serial.println(httpCode);
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        return false;
    }
}

// Function to calculate minutes until ETA from ISO 8601 timestamp
// Returns minutes difference (negative if ETA is in the past)
int calculateMinutesUntilETA(const String& etaStr) {
    // Parse ISO 8601 format: "2026-01-12T09:23:50+08:00"
    int tPos = etaStr.indexOf('T');
    if (tPos < 0) return -999; // Invalid format

    // Extract date part: "2026-01-12"
    String dateStr = etaStr.substring(0, tPos);
    if (dateStr.length() < 10) return -999;
    int year = dateStr.substring(0, 4).toInt();
    int month = dateStr.substring(5, 7).toInt();
    int day = dateStr.substring(8, 10).toInt();

    // Extract time part: "09:23:50"
    int plusPos = etaStr.indexOf('+', tPos);
    int minusPos = etaStr.indexOf('-', tPos + 1);
    int timeEndPos = (plusPos > 0) ? plusPos : ((minusPos > 0) ? minusPos : etaStr.length());
    String timeStr = etaStr.substring(tPos + 1, timeEndPos);

    if (timeStr.length() < 5) return -999;
    int hour = timeStr.substring(0, 2).toInt();
    int minute = timeStr.substring(3, 5).toInt();
    int second = (timeStr.length() >= 8) ? timeStr.substring(6, 8).toInt() : 0;

    // Get current time (store once to avoid timing issues)
    time_t now = time(nullptr);

    // Check if time is set (should be > 1000000000 for year 2001+)
    if (now < 1000000000) {
        // Time not synchronized, can't calculate difference accurately
        return -999;
    }

    struct tm timeinfo;
    if (!localtime_r(&now, &timeinfo)) {
        return -999;
    }

    // Create target time structure
    struct tm targetTime;
    targetTime.tm_year = year - 1900;
    targetTime.tm_mon = month - 1;
    targetTime.tm_mday = day;
    targetTime.tm_hour = hour;
    targetTime.tm_min = minute;
    targetTime.tm_sec = second;
    targetTime.tm_isdst = -1; // Let system determine DST

    // Convert to time_t
    time_t target = mktime(&targetTime);
    if (target == -1) return -999;

    // Calculate difference in seconds, then convert to minutes
    long diffSeconds = difftime(target, now);
    int diffMinutes = (int)(diffSeconds / 60);

    return diffMinutes;
}

// Function to format ETA for display with minutes until arrival
String formatETA(const String& etaStr) {
    // Extract time part from ISO 8601: "2026-01-11T23:30:00+08:00" -> "23:30"
    int pos = etaStr.indexOf('T');
    if (pos > 0) {
        int endPos = etaStr.indexOf('+', pos);
        if (endPos < 0) endPos = etaStr.indexOf('-', pos + 1);
        if (endPos > 0) {
            String timeStr = etaStr.substring(pos + 1, pos + 6); // Extract HH:MM

            // Calculate minutes until ETA
            int minutes = calculateMinutesUntilETA(etaStr);
            if (minutes >= 0 && minutes < 10000) { // Valid future time
                timeStr += " (";
                timeStr += String(minutes);
                timeStr += " min)";
            } else if (minutes < 0 && minutes > -10000) { // Past time
                timeStr += " (-";
                timeStr += String(-minutes);
                timeStr += " min)";
            }

            return timeStr;
        }
    }
    return etaStr;
}

// Function to display route info on Serial
void displayRouteInfoSerial(const RouteInfo& info) {
    Serial.println("\n=== Route Information ===");
    Serial.println("Company: " + info.company);
    Serial.println("Route: " + info.route);
    Serial.println("Bound: " + info.bound);
    Serial.println("Service Type: " + String(info.service_type));
    Serial.println("Origin (EN): " + info.origin_en);
    Serial.println("Origin (TC): " + info.origin_tc);
    Serial.println("Destination (EN): " + info.dest_en);
    Serial.println("Destination (TC): " + info.dest_tc);
    Serial.println("=======================\n");
}

// Function to display route info on e-ink display
void displayRouteInfoEInk(const RouteInfo& info) {
    display.clearMemory();

    // Set font
    u8g2_adapter.setFont(u8g2_font_helvR08_tf);

    int yPos = 10;
    int lineHeight = 12;

    // Display route number
    u8g2_adapter.setCursor(5, yPos);
    u8g2_adapter.print("Route: " + info.route);
    yPos += lineHeight;

    // Display origin
    u8g2_adapter.setCursor(5, yPos);
    u8g2_adapter.print("From: " + info.origin_en);
    yPos += lineHeight;

    // Display destination
    u8g2_adapter.setCursor(5, yPos);
    u8g2_adapter.print("To: " + info.dest_en);
    yPos += lineHeight;

    // Display bound
    u8g2_adapter.setCursor(5, yPos);
    u8g2_adapter.print("Bound: " + info.bound);
    yPos += lineHeight;

    // Display service type
    u8g2_adapter.setCursor(5, yPos);
    u8g2_adapter.print("Service: " + String(info.service_type));

    display.update();
}

// Function to display ETA info on Serial
void displayETASerial() {
    Serial.println("\n=== ETA Information ===");
    Serial.print("Route: ");
    Serial.print(appConfig.route);
    Serial.print(" | Stop: ");
    if (currentStopInfo.valid && currentStopInfo.name_en.length() > 0) {
        Serial.print(currentStopInfo.name_en);
        Serial.print(" (");
        Serial.print(selectedStopId);
        Serial.println(")");
    } else if (appConfig.stop_name.length() > 0) {
        Serial.print(appConfig.stop_name);
        Serial.print(" (");
        Serial.print(selectedStopId);
        Serial.println(")");
    } else {
        Serial.println(selectedStopId);
    }

    if (etaCount == 0) {
        Serial.println("No ETAs available");
    } else {
        for (int i = 0; i < etaCount; i++) {
            Serial.print("Bus ");
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.print(formatETA(etaList[i].eta));
            Serial.print(" (");
            Serial.print(etaList[i].remark_en);
            Serial.print(") -> ");
            Serial.println(etaList[i].dest_en);
        }
    }
    Serial.println("=======================\n");
}

// Function to format last updated time as full timestamp (GMT+8)
String formatLastUpdated(unsigned long fetchTime) {
    if (fetchTime == 0) {
        return "No update";
    }

    // Get current time
    time_t now = time(nullptr);

    time_t targetTime;
    // Calculate the actual time when fetch happened.
    unsigned long secondsAgo = (millis() - fetchTime) / 1000;
    targetTime = now - secondsAgo;

    if (now < 1000000000) {
        // Time not synchronized, show relative time
        unsigned long secondsAgo = (millis() - fetchTime) / 1000;
        if (secondsAgo < 60) {
            return String(secondsAgo) + "s ago";
        } else if (secondsAgo < 3600) {
            return String(secondsAgo / 60) + "m ago";
        } else {
            return String(secondsAgo / 3600) + "h ago";
        }
    }

    // Use localtime_r which respects the timezone set by configTime (GMT+8)
    struct tm timeinfo;
    if (!localtime_r(&targetTime, &timeinfo)) {
        // Fallback to relative time if localtime_r fails
        unsigned long secondsAgo = (millis() - fetchTime) / 1000;
        if (secondsAgo < 60) {
            return String(secondsAgo) + "s ago";
        } else if (secondsAgo < 3600) {
            return String(secondsAgo / 60) + "m ago";
        } else {
            return String(secondsAgo / 3600) + "h ago";
        }
    }

    // Format as "HH:MM:SS HKT" (GMT+8 timezone)
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d HKT",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(timeStr);
}

// Function to display ETA info on e-ink display
void displayETAEInk() {
    display.clearMemory();

    u8g2_adapter.setFont(u8g2_font_helvR08_tf);

    String stopLabel = selectedStopId;
    if (currentStopInfo.valid && currentStopInfo.name_en.length() > 0) {
        stopLabel = currentStopInfo.name_en;
    } else if (appConfig.stop_name.length() > 0) {
        stopLabel = appConfig.stop_name;
    }
    if (stopLabel.length() > 25) {
        stopLabel = stopLabel.substring(0, 22) + "...";
    }

    String header = appConfig.route + " @ " + stopLabel;
    if (header.length() > 28) {
        header = header.substring(0, 25) + "...";
    }
    u8g2_adapter.setCursor(5, 10);
    u8g2_adapter.print(header);

    String dest = etaCount > 0 ? etaList[0].dest_en : currentRouteInfo.dest_en;
    if (dest.length() == 0) dest = "Destination";
    dest = "To " + dest;
    if (dest.length() > 28) {
        dest = dest.substring(0, 25) + "...";
    }
    u8g2_adapter.setCursor(5, 24);
    u8g2_adapter.print(dest);

    if (etaCount == 0) {
        u8g2_adapter.setFont(u8g2_font_helvB14_tf);
        u8g2_adapter.setCursor(5, 54);
        u8g2_adapter.print("No ETA");
        u8g2_adapter.setFont(u8g2_font_helvR08_tf);
        u8g2_adapter.setCursor(5, 76);
        u8g2_adapter.print("Will retry");
    } else {
        int firstMinutes = calculateMinutesUntilETA(etaList[0].eta);
        String status = "NEXT BUS";
        if (firstMinutes <= 0 && firstMinutes > -10000) {
            status = "DUE NOW";
        } else if (firstMinutes > 0 && firstMinutes <= appConfig.alert_minutes) {
            status = "BUS SOON";
        }

        u8g2_adapter.setFont(u8g2_font_helvB14_tf);
        u8g2_adapter.setCursor(5, 48);
        u8g2_adapter.print(status);

        u8g2_adapter.setFont(u8g2_font_helvR12_tf);
        u8g2_adapter.setCursor(5, 70);
        String firstBus = "#1 " + formatETA(etaList[0].eta);
        if (firstBus.length() > 22) {
            firstBus = firstBus.substring(0, 19) + "...";
        }
        u8g2_adapter.print(firstBus);

        u8g2_adapter.setFont(u8g2_font_helvR08_tf);
        int yPos = 88;
        for (int i = 1; i < etaCount && i < MAX_ETAS; i++) {
            u8g2_adapter.setCursor(5, yPos);
            String busInfo = "#" + String(i + 1) + "  " + formatETA(etaList[i].eta);
            if (busInfo.length() > 28) {
                busInfo = busInfo.substring(0, 25) + "...";
            }
            u8g2_adapter.print(busInfo);
            yPos += 14;
        }
    }

    u8g2_adapter.setFont(u8g2_font_helvR08_tf);
    u8g2_adapter.setCursor(5, display.height() - 8);
    String footer = "Updated: " + formatLastUpdated(lastETAFetchTime);
    if (footer.length() > 30) {
        footer = footer.substring(0, 27) + "...";
    }
    u8g2_adapter.print(footer);

    display.update();
}

String formatETACompact(const String& etaStr) {
    int tPos = etaStr.indexOf('T');
    String timeStr = etaStr;
    if (tPos > 0 && etaStr.length() >= tPos + 6) {
        timeStr = etaStr.substring(tPos + 1, tPos + 6);
    }

    int minutes = calculateMinutesUntilETA(etaStr);
    if (minutes <= 0 && minutes > -10000) {
        return "Due now " + timeStr;
    }
    if (minutes > 0 && minutes < 10000) {
        return String(minutes) + " min " + timeStr;
    }
    return timeStr;
}

void copyActiveRouteState(RouteDisplayState& state, RouteFetchStatus status, const String& errorText) {
    state.enabled = true;
    state.status = status;
    state.route = appConfig.route;
    state.bound = appConfig.bound;
    state.service_type = appConfig.service_type;
    state.stop_seq = selectedStopSeq;
    state.stop_id = selectedStopId;
    state.station_name = currentStopInfo.valid && currentStopInfo.name_en.length() > 0 ? currentStopInfo.name_en : appConfig.stop_name;
    if (state.station_name.length() == 0) {
        state.station_name = selectedStopId;
    }
    state.dest_en = etaCount > 0 ? etaList[0].dest_en : currentRouteInfo.dest_en;
    state.error_text = errorText;
    state.eta_count = etaCount;
    state.fetch_time = lastETAFetchTime;
    for (int i = 0; i < MAX_ETAS; i++) {
        if (i < etaCount) {
            state.etas[i] = etaList[i];
        } else {
            state.etas[i].valid = false;
        }
    }
}

void displayRouteStatesSerial() {
    Serial.println("\n=== Bus Tag Summary ===");
    Serial.print("Updated: ");
    Serial.println(formatLastUpdated(lastETAFetchTime));

    for (int i = 0; i < routeStateCount; i++) {
        RouteDisplayState& state = routeStates[i];
        Serial.print("Route ");
        Serial.print(state.route);
        Serial.print(" ");
        Serial.print(state.bound);
        Serial.print(": ");

        if (state.status == ROUTE_STATUS_OK && state.eta_count > 0) {
            Serial.print(formatETA(state.etas[0].eta));
            if (state.dest_en.length() > 0) {
                Serial.print(" to ");
                Serial.print(state.dest_en);
            }
            Serial.print(" @ ");
            Serial.println(state.station_name);
        } else if (state.status == ROUTE_STATUS_NO_SERVICE) {
            Serial.println("Outside hours / no active ETA");
        } else {
            Serial.println(state.error_text.length() > 0 ? state.error_text : "Fetch failed");
        }
    }
    Serial.println("=======================\n");
}

String displayHeaderStationLabel() {
    String firstStation;
    bool allSame = true;
    for (int i = 0; i < routeStateCount; i++) {
        String station = routeStates[i].station_name;
        if (station.length() == 0) continue;
        if (firstStation.length() == 0) {
            firstStation = station;
        } else if (station != firstStation) {
            allSame = false;
            break;
        }
    }

    if (allSame && firstStation.length() > 0) return firstStation;
    return "Bus ETA";
}

String routeStatusLine(const RouteDisplayState& state) {
    if (state.status == ROUTE_STATUS_OK && state.eta_count > 0 && state.etas[0].valid) {
        return formatETACompact(state.etas[0].eta);
    }
    if (state.status == ROUTE_STATUS_NO_SERVICE) {
        return "Outside hours";
    }
    return "No data";
}

void displayRouteStatesEInk() {
    display.clearMemory();
    u8g2_adapter.setFont(u8g2_font_helvR08_tf);

    String stopLabel = displayHeaderStationLabel();
    if (stopLabel.length() == 0) stopLabel = "Bus stop";
    if (stopLabel.length() > 28) {
        stopLabel = stopLabel.substring(0, 25) + "...";
    }

    u8g2_adapter.setCursor(5, 10);
    u8g2_adapter.print(stopLabel);

    int yPos = 32;
    for (int i = 0; i < routeStateCount && i < MAX_CONFIGURED_ROUTES; i++) {
        RouteDisplayState& state = routeStates[i];
        u8g2_adapter.setFont(u8g2_font_helvB12_tf);
        u8g2_adapter.setCursor(5, yPos);
        String routeLabel = state.route;
        if (routeLabel.length() > 6) {
            routeLabel = routeLabel.substring(0, 6);
        }
        u8g2_adapter.print(routeLabel);

        u8g2_adapter.setFont(u8g2_font_helvR10_tf);
        u8g2_adapter.setCursor(62, yPos);
        String status = routeStatusLine(state);
        if (status.length() > 22) {
            status = status.substring(0, 19) + "...";
        }
        u8g2_adapter.print(status);

        u8g2_adapter.setFont(u8g2_font_helvR08_tf);
        u8g2_adapter.setCursor(62, yPos + 12);
        String detail = state.station_name;
        if (detail.length() == 0 && state.status == ROUTE_STATUS_OK && state.dest_en.length() > 0) {
            detail = "To " + state.dest_en;
        } else if (detail.length() == 0) {
            detail = state.error_text;
        }
        if (state.status == ROUTE_STATUS_NO_SERVICE) {
            detail = state.station_name.length() > 0 ? state.station_name : "No scheduled bus now";
        }
        if (detail.length() > 26) {
            detail = detail.substring(0, 23) + "...";
        }
        u8g2_adapter.print(detail);

        yPos += 28;
    }

    u8g2_adapter.setFont(u8g2_font_helvR08_tf);
    u8g2_adapter.setCursor(5, display.height() - 8);
    String footer = "Updated: " + formatLastUpdated(lastETAFetchTime);
    if (footer.length() > 30) {
        footer = footer.substring(0, 27) + "...";
    }
    u8g2_adapter.print(footer);

    display.update();
}

bool refreshActiveRouteData(RouteDisplayState& state) {
    currentRouteInfo.valid = false;
    currentStopInfo.valid = false;
    etaCount = 0;
    selectedStopSeq = appConfig.stop_seq;
    selectedStopId = appConfig.stop_id.length() > 0 ? appConfig.stop_id : String(STOP_ID);

    Serial.println("\nAttempting to fetch route information...");
    Serial.print("Configured route: ");
    Serial.print(appConfig.route);
    Serial.print(", bound=");
    Serial.print(appConfig.bound);
    Serial.print(", service=");
    Serial.print(appConfig.service_type);
    Serial.print(", station=");
    Serial.print(appConfig.stop_name.length() > 0 ? appConfig.stop_name : String(appConfig.stop_seq));
    Serial.print(", stop seq=");
    Serial.println(appConfig.stop_seq);
    Serial.flush();
    showStatusMessage("Fetching route", appConfig.route + " " + appConfig.bound, appConfig.stop_name);

    bool resolvedFromStation = false;
    if (appConfig.stop_name.length() > 0 && appConfig.stop_id.length() == 0) {
        resolvedFromStation = resolveRouteStopFromName(appConfig.route.c_str());
        if (!resolvedFromStation) {
            copyActiveRouteState(state, ROUTE_STATUS_ERROR, "Check route/station");
            return false;
        }
    }

    if (!currentRouteInfo.valid && !fetchRouteInfo(appConfig.route.c_str(), appConfig.bound.c_str(), appConfig.service_type)) {
        fetchRouteInfoWithRetry(appConfig.route.c_str());
    }

    int serviceType = currentRouteInfo.valid ? currentRouteInfo.service_type : appConfig.service_type;
    String boundToUse = currentRouteInfo.valid ? currentRouteInfo.bound : appConfig.bound;
    boundToUse = normalizeBound(boundToUse);

    String stopFromSeq;
    bool resolvedStop = false;
    if (!resolvedFromStation && appConfig.stop_name.length() > 0 && appConfig.stop_id.length() == 0) {
        resolvedStop = resolveStopFromName(appConfig.route.c_str(), boundToUse.c_str(), serviceType);
    }

    if (resolvedFromStation || resolvedStop) {
        Serial.println("Using station resolved from human-readable name");
    } else if (appConfig.stop_id.length() > 0) {
        selectedStopId = appConfig.stop_id;
    } else if (selectStopIdBySeq(appConfig.route.c_str(), boundToUse.c_str(), serviceType, selectedStopSeq, stopFromSeq)) {
        selectedStopId = stopFromSeq;
    } else {
        selectedStopId = String(STOP_ID);
    }

    Serial.println("\nAttempting to fetch stop information...");
    Serial.flush();
    if (!currentStopInfo.valid || currentStopInfo.stop_id != selectedStopId) {
        showStatusMessage("Loading station", appConfig.stop_name.length() > 0 ? appConfig.stop_name : selectedStopId, appConfig.route);
        fetchStopInfo(selectedStopId.c_str());
    }

    Serial.println("\nAttempting to fetch ETA information...");
    Serial.flush();
    showStatusMessage("Fetching ETA", appConfig.route, currentStopInfo.valid ? currentStopInfo.name_en : appConfig.stop_name);

    bool etaSuccess = false;
    bool sawApiResponse = false;
    bool sawNoService = false;
    if (fetchRouteETAInfo(appConfig.route.c_str(), serviceType, selectedStopSeq, boundToUse.c_str())) {
        etaSuccess = true;
    } else {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (!etaSuccess && serviceType != DEFAULT_SERVICE_TYPE && fetchRouteETAInfo(appConfig.route.c_str(), DEFAULT_SERVICE_TYPE, selectedStopSeq, boundToUse.c_str())) {
        etaSuccess = true;
    } else if (!etaSuccess && serviceType != DEFAULT_SERVICE_TYPE) {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (!etaSuccess && serviceType != 2 && fetchRouteETAInfo(appConfig.route.c_str(), 2, selectedStopSeq, boundToUse.c_str())) {
        etaSuccess = true;
    } else if (!etaSuccess && serviceType != 2) {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (!etaSuccess && fetchETAInfo(appConfig.route.c_str(), selectedStopId.c_str(), serviceType)) {
        etaSuccess = true;
    } else if (!etaSuccess) {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (!etaSuccess && serviceType != DEFAULT_SERVICE_TYPE && fetchETAInfo(appConfig.route.c_str(), selectedStopId.c_str(), DEFAULT_SERVICE_TYPE)) {
        etaSuccess = true;
    } else if (!etaSuccess && serviceType != DEFAULT_SERVICE_TYPE) {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (!etaSuccess && serviceType != 2 && fetchETAInfo(appConfig.route.c_str(), selectedStopId.c_str(), 2)) {
        etaSuccess = true;
    } else if (!etaSuccess && serviceType != 2) {
        sawApiResponse = sawApiResponse || lastETARequestApiOk;
        sawNoService = sawNoService || lastETARequestNoService;
    }

    if (etaSuccess) {
        Serial.println("Successfully fetched ETA info!");
        displayETASerial();
        copyActiveRouteState(state, ROUTE_STATUS_OK, "");
    } else {
        RouteFetchStatus status = (sawNoService || sawApiResponse) ? ROUTE_STATUS_NO_SERVICE : ROUTE_STATUS_ERROR;
        if (status == ROUTE_STATUS_NO_SERVICE) {
            Serial.println("No active ETA. Treating this as outside operation time or no scheduled bus now.");
        } else {
            Serial.println("ERROR: Failed to fetch ETA information");
            Serial.println("Check WiFi connection and API availability");
        }
        Serial.flush();
        String errorStation = currentStopInfo.valid ? currentStopInfo.name_en : appConfig.stop_name;
        if (errorStation.length() == 0) {
            errorStation = "stop " + String(selectedStopSeq);
        }
        if (status == ROUTE_STATUS_NO_SERVICE) {
            showStatusMessage("Outside hours", appConfig.route + " " + errorStation, "Will retry later");
            copyActiveRouteState(state, status, "Outside hours");
        } else {
            showStatusMessage("ETA API Error", appConfig.route + " " + errorStation, "Will retry later");
            copyActiveRouteState(state, status, "Fetch failed");
        }
    }

    return etaSuccess;
}

bool refreshBusData() {
    routeStateCount = 0;
    bool anyEta = false;
    bool anyRouteAnswered = false;

    for (int routeIndex = 0; routeIndex < MAX_CONFIGURED_ROUTES; routeIndex++) {
        if (!isRouteEnabled(routeIndex)) continue;

        applyRouteConfigToActive(routeIndex);
        RouteDisplayState& state = routeStates[routeStateCount];
        bool etaSuccess = refreshActiveRouteData(state);
        anyEta = anyEta || etaSuccess;
        anyRouteAnswered = anyRouteAnswered || state.status == ROUTE_STATUS_OK || state.status == ROUTE_STATUS_NO_SERVICE;
        routeStateCount++;
    }

    if (routeStateCount == 0) {
        showStatusMessage("No route setup", "Open setup portal", "Press setup button");
        return false;
    }

    plannedSleepSeconds = calculateNextSleepSeconds();
    displayRouteStatesSerial();
    displayRouteStatesEInk();
    applyRouteConfigToActive(0);

    return anyEta || anyRouteAnswered;
}

uint32_t calculateNextSleepSeconds() {
    uint32_t normalSleep = appConfig.sleep_minutes * 60;
    int soonestMinutes = 10001;
    for (int i = 0; i < routeStateCount; i++) {
        RouteDisplayState& state = routeStates[i];
        if (state.status != ROUTE_STATUS_OK || state.eta_count == 0 || !state.etas[0].valid) continue;
        int routeMinutes = calculateMinutesUntilETA(state.etas[0].eta);
        if (routeMinutes >= 0 && routeMinutes < soonestMinutes) {
            soonestMinutes = routeMinutes;
        }
    }

    if (soonestMinutes > 10000) return normalSleep;

    if (soonestMinutes <= appConfig.alert_minutes) {
        return 60;
    }

    int minutesUntilAlertWindow = soonestMinutes - appConfig.alert_minutes;
    if (minutesUntilAlertWindow <= 2) {
        return 60;
    }

    uint32_t secondsUntilAlertWindow = minutesUntilAlertWindow * 60;
    if (secondsUntilAlertWindow < normalSleep) {
        return secondsUntilAlertWindow;
    }

    return normalSleep;
}

void enterDeepSleep(uint32_t sleepSeconds) {
#ifdef STAY_AWAKE_FOR_DEBUG
    Serial.println("STAY_AWAKE_FOR_DEBUG is set; staying awake");
    return;
#else
    if (sleepSeconds < 60) sleepSeconds = 60;

    Serial.print("Entering deep sleep for ");
    Serial.print(sleepSeconds);
    Serial.println(" seconds");
    Serial.println("Press the setup button while asleep to reopen the setup portal.");
    Serial.flush();

    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    btStop();
    delay(100);

    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * US_PER_SECOND);
    esp_err_t wakeButtonResult = esp_sleep_enable_ext0_wakeup((gpio_num_t)SETUP_BUTTON_PIN, SETUP_BUTTON_ACTIVE_LEVEL);
    if (wakeButtonResult != ESP_OK) {
        Serial.print("Setup button wake not enabled, esp_err=");
        Serial.println((int)wakeButtonResult);
        Serial.flush();
    }

    esp_deep_sleep_start();
#endif
}

String htmlEscape(String value) {
    value.replace("&", "&amp;");
    value.replace("<", "&lt;");
    value.replace(">", "&gt;");
    value.replace("\"", "&quot;");
    value.replace("'", "&#39;");
    return value;
}

String routeFieldName(const char* base, int routeIndex) {
    if (routeIndex == 0) return String(base);
    return String(base) + String(routeIndex + 1);
}

String routeDirectionLabel(const RouteVariant& variant) {
    String label = variant.bound;
    label += " ";
    label += variant.origin_en.length() > 0 ? variant.origin_en : variant.origin_tc;
    label += " -> ";
    label += variant.dest_en.length() > 0 ? variant.dest_en : variant.dest_tc;
    return label;
}

String routeChoiceButtonHtml(int routeIndex, const RouteVariant& variant) {
    String button;
    button += F("<button type='button' class='route-choice' data-i='");
    button += String(routeIndex);
    button += F("' data-route='");
    button += htmlEscape(variant.route);
    button += F("' data-bound='");
    button += htmlEscape(variant.bound);
    button += F("' data-service='");
    button += String(variant.service_type);
    button += F("' data-label='");
    button += htmlEscape(routeDirectionLabel(variant));
    button += F("'>");
    button += htmlEscape(variant.route + " " + routeDirectionLabel(variant));
    button += F("</button><br>");
    return button;
}

String stopChoiceButtonHtml(int routeIndex, const StopCandidate& candidate, const StopInfo& info, bool first) {
    String name = info.name_en.length() > 0 ? info.name_en : info.name_tc;
    if (name.length() == 0) name = candidate.stop_id;

    String button;
    button += F("<button type='button' class='stop-choice'");
    if (first) button += F(" data-first='1'");
    button += F(" data-i='");
    button += String(routeIndex);
    button += F("' data-stopid='");
    button += htmlEscape(candidate.stop_id);
    button += F("' data-seq='");
    button += String(candidate.seq);
    button += F("' data-name='");
    button += htmlEscape(name);
    button += F("'>");
    button += String(candidate.seq);
    button += F(". ");
    button += htmlEscape(name);
    button += F("</button><br>");
    return button;
}

String routeSearchResultsHtml(int routeIndex, const String& rawQuery) {
    String query = rawQuery;
    query.trim();
    query.toUpperCase();
    if (query.length() == 0) return "Enter a route number first.";

    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    String url = String(API_BASE_URL) + "/v1/transport/kmb/route/";
    http.begin(client, url);
    http.setTimeout(20000);
    http.addHeader("User-Agent", "ESP32-KMB-Route-Search-UI/1.0");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        return "Could not load KMB route table.";
    }

    WiFiClient* stream = http.getStreamPtr();
    String marker;
    marker.reserve(12);
    String objectJson;
    objectJson.reserve(512);
    String html;
    html.reserve(5000);

    bool inDataArray = false;
    bool capturing = false;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    int count = 0;
    unsigned long lastByteAt = millis();

    while ((http.connected() || stream->available()) && count < MAX_ROUTE_SEARCH_CHOICES) {
        if (!stream->available()) {
            if (millis() - lastByteAt > 20000) break;
            delay(1);
            continue;
        }

        char c = (char)stream->read();
        lastByteAt = millis();

        if (!inDataArray) {
            marker += c;
            if (marker.length() > 8) marker.remove(0, marker.length() - 8);
            if (marker.endsWith("\"data\":[")) inDataArray = true;
            continue;
        }

        if (!capturing) {
            if (c == ']') break;
            if (c == '{') {
                capturing = true;
                inString = false;
                escaped = false;
                depth = 1;
                objectJson = "{";
            }
            continue;
        }

        objectJson += c;
        if (escaped) {
            escaped = false;
        } else if (inString && c == '\\') {
            escaped = true;
        } else if (c == '"') {
            inString = !inString;
        } else if (!inString) {
            if (c == '{') depth++;
            if (c == '}') depth--;
        }

        if (objectJson.length() > 768) {
            capturing = false;
            objectJson = "";
            depth = 0;
            continue;
        }

        if (depth == 0) {
            RouteVariant variant;
            if (parseRouteVariantObject(objectJson, variant) && variant.route.indexOf(query) >= 0) {
                html += routeChoiceButtonHtml(routeIndex, variant);
                count++;
            }
            capturing = false;
            objectJson = "";
        }
    }

    http.end();

    if (count == 0) {
        return "No matching KMB route found.";
    }

    html = "<div class='hint'>Choose route and direction. Keep typing to narrow a long list.</div>" + html;
    if (count >= MAX_ROUTE_SEARCH_CHOICES) {
        html += "<div class='hint'>More matches exist. Type more digits/letters to narrow.</div>";
    }
    return html;
}

String stopSearchResultsHtml(int routeIndex, const String& route, const String& bound, int serviceType) {
    if (route.length() == 0 || bound.length() == 0 || serviceType <= 0) {
        return "Choose a route and direction first.";
    }

    StopCandidate candidates[MAX_ROUTE_STOPS];
    int candidateCount = 0;
    if (!fetchRouteStopCandidates(route.c_str(), bound.c_str(), serviceType, candidates, candidateCount)) {
        return "Could not load stops for this direction.";
    }

    String html = "<div class='hint'>Choose the stop where you wait.</div>";
    html.reserve(7000);
    for (int i = 0; i < candidateCount; i++) {
        StopInfo info;
        if (!fetchStopInfoInto(candidates[i].stop_id.c_str(), info)) continue;
        html += stopChoiceButtonHtml(routeIndex, candidates[i], info, i == 0);
    }
    return html;
}

String busSetupPage(const String& message) {
    String page;
    page.reserve(5000);
    page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
    page += F("<title>Bus ETA setup</title><style>");
    page += F("body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:20px;max-width:760px;color:#111}label{display:block;margin-top:14px;font-weight:650}input{box-sizing:border-box;width:100%;font-size:18px;padding:10px;margin-top:6px}button{font-size:16px;padding:9px 12px;margin:8px 6px 0 0}.hint{color:#555;font-size:14px}.msg{padding:10px;background:#eef6ff;border:1px solid #9cc7ff;margin:12px 0}.route-box{border-top:1px solid #ddd;margin-top:18px;padding-top:12px}.choice{background:#f6f6f6;padding:10px;margin-top:8px}</style>");
    page += F("</head><body><h1>Bus ETA setup</h1>");
    if (message.length() > 0) {
        page += F("<div class='msg'>");
        page += htmlEscape(message);
        page += F("</div>");
    }
    page += F("<form method='post' action='/save'>");
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        String suffix = i == 0 ? "" : String(i + 1);
        page += F("<div class='route-box'><h2>Route ");
        page += String(i + 1);
        if (i > 0) page += F(" optional");
        page += F("</h2>");
        page += F("<label>Bus route<input ");
        if (i == 0) page += F("id='route' ");
        page += F("name='route");
        page += suffix;
        page += F("' oninput='clearRouteChoice(");
        page += String(i);
        page += F(")' value='");
        page += htmlEscape(appConfig.routes[i].route);
        page += F("' autocomplete='off'></label>");
        page += F("<input type='hidden' name='");
        page += routeFieldName("bound", i);
        page += F("' value='");
        page += htmlEscape(appConfig.routes[i].bound);
        page += F("'><input type='hidden' name='");
        page += routeFieldName("service", i);
        page += F("' value='");
        page += String(appConfig.routes[i].service_type);
        page += F("'><input type='hidden' name='");
        page += routeFieldName("stopseq", i);
        page += F("' value='");
        page += String(appConfig.routes[i].stop_seq);
        page += F("'><input type='hidden' name='");
        page += routeFieldName("stopid", i);
        page += F("' value='");
        page += htmlEscape(appConfig.routes[i].stop_id);
        page += F("'>");
        page += F("<button type='button' onclick='searchRoute(");
        page += String(i);
        page += F(")'>Search route/direction</button>");
        page += F("<div class='hint' id='direction");
        page += String(i);
        page += F("'>Selected direction: ");
        page += htmlEscape(appConfig.routes[i].bound + " service " + String(appConfig.routes[i].service_type));
        page += F("</div><div class='choice' id='routes");
        page += String(i);
        page += F("'></div>");
        page += F("<label>Station / stop for this route<input name='stopname");
        page += suffix;
        page += F("' oninput='clearStopChoice(");
        page += String(i);
        page += F(")' value='");
        page += htmlEscape(appConfig.routes[i].stop_name);
        page += F("' autocomplete='off'></label>");
        page += F("<button type='button' onclick='loadStops(");
        page += String(i);
        page += F(")'>Choose stop from selected direction</button>");
        page += F("<div class='choice' id='stops");
        page += String(i);
        page += F("'></div></div>");
    }
    page += F("<div class='hint'>Search route, choose the direction, then choose the stop where you wait. If no stop is chosen, the tag tries to resolve the station text automatically.</div>");
    page += F("<label>Sleep minutes<input name='sleepmin' type='number' min='1' max='60' value='");
    page += String(appConfig.sleep_minutes);
    page += F("'></label>");
    page += F("<label>Alert minutes<input name='alertmin' type='number' min='1' max='30' value='");
    page += String(appConfig.alert_minutes);
    page += F("'></label>");
    page += F("<button type='submit'>Save bus setup</button></form>");
    page += F("<script>");
    page += F("function fname(b,i){return i===0?b:b+(i+1)}function el(n,i){return document.getElementsByName(fname(n,i))[0]}");
    page += F("function clearRouteChoice(i){el('bound',i).value='';el('service',i).value='';clearStopChoice(i);document.getElementById('direction'+i).textContent='Selected direction: none';}");
    page += F("function clearStopChoice(i){el('stopid',i).value='';el('stopseq',i).value='';}");
    page += F("function searchRoute(i){var r=el('route',i).value.trim();var box=document.getElementById('routes'+i);box.textContent='Searching...';fetch('/routes?i='+i+'&q='+encodeURIComponent(r)).then(x=>x.text()).then(t=>{box.innerHTML=t;box.querySelectorAll('.route-choice').forEach(b=>b.onclick=()=>chooseRoute(i,b.dataset.route,b.dataset.bound,b.dataset.service,b.dataset.label));});}");
    page += F("function chooseRoute(i,r,b,s,l){el('route',i).value=r;el('bound',i).value=b;el('service',i).value=s;el('stopid',i).value='';el('stopseq',i).value='';document.getElementById('direction'+i).textContent='Selected direction: '+l;loadStops(i);}");
    page += F("function loadStops(i){var r=el('route',i).value.trim(),b=el('bound',i).value,s=el('service',i).value;var box=document.getElementById('stops'+i);box.textContent='Loading stops...';fetch('/stops?i='+i+'&route='+encodeURIComponent(r)+'&bound='+encodeURIComponent(b)+'&service='+encodeURIComponent(s)).then(x=>x.text()).then(t=>{box.innerHTML=t;box.querySelectorAll('.stop-choice').forEach(btn=>btn.onclick=()=>chooseStop(i,btn.dataset.stopid,btn.dataset.seq,btn.dataset.name));var st=el('stopname',i),sid=el('stopid',i);var first=box.querySelector('.stop-choice[data-first=\"1\"]');if(first&&!st.value&&!sid.value)chooseStop(i,first.dataset.stopid,first.dataset.seq,first.dataset.name);});}");
    page += F("function chooseStop(i,id,seq,name){el('stopid',i).value=id;el('stopseq',i).value=seq;el('stopname',i).value=name;}");
    page += F("</script>");
    page += F("</body></html>");
    return page;
}

void clearResolvedRouteIfChanged(int routeIndex, const RouteConfig& previous, const String& previousStopName) {
    RouteConfig& current = appConfig.routes[routeIndex];
    bool routeChanged =
        current.route != previous.route ||
        current.stop_name != previousStopName;
    if (current.route.length() == 0) {
        current.stop_id = "";
        return;
    }
    if (routeChanged && current.stop_id.length() == 0) {
        current.stop_id = "";
        current.bound = DEFAULT_BOUND;
        current.service_type = DEFAULT_SERVICE_TYPE;
        current.stop_seq = DEFAULT_STOP_SEQ;
    }
}

void handleBusSetupRoot() {
    busSetupServer.send(200, "text/html", busSetupPage(""));
}

void handleBusSetupRoutes() {
    int routeIndex = busSetupServer.arg("i").toInt();
    if (routeIndex < 0 || routeIndex >= MAX_CONFIGURED_ROUTES) routeIndex = 0;
    busSetupServer.send(200, "text/html", routeSearchResultsHtml(routeIndex, busSetupServer.arg("q")));
}

void handleBusSetupStops() {
    int routeIndex = busSetupServer.arg("i").toInt();
    if (routeIndex < 0 || routeIndex >= MAX_CONFIGURED_ROUTES) routeIndex = 0;
    String route = busSetupServer.arg("route");
    route.trim();
    route.toUpperCase();
    String bound = normalizeBound(busSetupServer.arg("bound"));
    int serviceType = busSetupServer.arg("service").toInt();
    if (serviceType <= 0) serviceType = DEFAULT_SERVICE_TYPE;
    busSetupServer.send(200, "text/html", stopSearchResultsHtml(routeIndex, route, bound, serviceType));
}

void handleBusSetupSave() {
    RouteConfig previousRoutes[MAX_CONFIGURED_ROUTES];
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        previousRoutes[i] = appConfig.routes[i];
    }
    String previousStopNames[MAX_CONFIGURED_ROUTES];
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        previousStopNames[i] = appConfig.routes[i].stop_name;
    }

    appConfig.routes[0].route = busSetupServer.arg("route");
    appConfig.routes[0].bound = busSetupServer.arg("bound");
    appConfig.routes[0].service_type = busSetupServer.arg("service").toInt();
    appConfig.routes[0].stop_seq = busSetupServer.arg("stopseq").toInt();
    appConfig.routes[0].stop_id = busSetupServer.arg("stopid");
    appConfig.routes[0].stop_name = busSetupServer.arg("stopname");
    appConfig.routes[1].route = busSetupServer.arg("route2");
    appConfig.routes[1].bound = busSetupServer.arg("bound2");
    appConfig.routes[1].service_type = busSetupServer.arg("service2").toInt();
    appConfig.routes[1].stop_seq = busSetupServer.arg("stopseq2").toInt();
    appConfig.routes[1].stop_id = busSetupServer.arg("stopid2");
    appConfig.routes[1].stop_name = busSetupServer.arg("stopname2");
    appConfig.routes[2].route = busSetupServer.arg("route3");
    appConfig.routes[2].bound = busSetupServer.arg("bound3");
    appConfig.routes[2].service_type = busSetupServer.arg("service3").toInt();
    appConfig.routes[2].stop_seq = busSetupServer.arg("stopseq3").toInt();
    appConfig.routes[2].stop_id = busSetupServer.arg("stopid3");
    appConfig.routes[2].stop_name = busSetupServer.arg("stopname3");
    appConfig.sleep_minutes = busSetupServer.arg("sleepmin").toInt();
    appConfig.alert_minutes = busSetupServer.arg("alertmin").toInt();

    sanitizeAppConfig();
    for (int i = 0; i < MAX_CONFIGURED_ROUTES; i++) {
        clearResolvedRouteIfChanged(i, previousRoutes[i], previousStopNames[i]);
    }
    saveAppConfig();
    applyRouteConfigToActive(0);
    busSetupSaved = true;

    Serial.println("Saved bus setup from LAN web server");
    Serial.println("Routes: " + configuredRoutesLabel());
    Serial.println("Station 1: " + appConfig.routes[0].stop_name);
    Serial.println("Station 2: " + appConfig.routes[1].stop_name);
    Serial.println("Station 3: " + appConfig.routes[2].stop_name);
    Serial.flush();
    showStatusMessage("Bus setup saved", configuredRoutesLabel(), appConfig.stop_name);
    busSetupServer.send(200, "text/html", busSetupPage("Saved. The tag will validate route and station now."));
}

bool runBusSetupServer(uint32_t seconds) {
    if (WiFi.status() != WL_CONNECTED) return false;

    busSetupSaved = false;
    busSetupServer.on("/", HTTP_GET, handleBusSetupRoot);
    busSetupServer.on("/routes", HTTP_GET, handleBusSetupRoutes);
    busSetupServer.on("/stops", HTTP_GET, handleBusSetupStops);
    busSetupServer.on("/save", HTTP_POST, handleBusSetupSave);
    busSetupServer.onNotFound(handleBusSetupRoot);
    busSetupServer.begin();
    busSetupServerStarted = true;

    String ip = WiFi.localIP().toString();
    Serial.println("Bus setup server ready at http://" + ip + "/");
    Serial.flush();
    showStatusMessage("Bus setup", String("http://") + ip, "Use normal WiFi");

    unsigned long start = millis();
    while (millis() - start < seconds * 1000UL && !busSetupSaved) {
        busSetupServer.handleClient();
        if (otaStarted) {
            ArduinoOTA.handle();
        }
        delay(5);
    }

    busSetupServer.stop();
    busSetupServerStarted = false;
    return busSetupSaved;
}

void setup() {
    Serial.begin(115200);
    delay(300);

    // Wait for Serial to be available (for USB CDC)
    while (!Serial && millis() < 1200) {
        delay(10);
    }

    Serial.println("\n\n========================================");
    Serial.println("KMB Bus ETA Tag");
    Serial.println("========================================");
    Serial.println("Initializing...");
    Serial.flush();

    pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

    // Initialize e-ink display
    display.landscape();
    u8g2_adapter.begin();

    loadAppConfig();

    bool forcePortal = shouldOpenConfigPortal();
    if (!forcePortal) {
        Serial.println("Using saved setup:");
        Serial.print("  routes=");
        Serial.print(configuredRoutesLabel());
        Serial.print(" (");
        Serial.print(configuredRouteCount());
        Serial.print(" configured)");
        Serial.print(", station=");
        Serial.print(appConfig.stop_name.length() > 0 ? appConfig.stop_name : String(appConfig.routes[0].stop_seq));
        Serial.print(", sleep_min=");
        Serial.print(appConfig.sleep_minutes);
        Serial.print(", alert_min=");
        Serial.println(appConfig.alert_minutes);
    }

    if (!connectWiFiAndMaybeConfigure(forcePortal)) {
        showStatusMessage("WiFi unavailable", "Setup: press button", "Retrying later");
        enterDeepSleep(appConfig.sleep_minutes * 60);
        return;
    }

    // Configure time for Hong Kong (UTC+8)
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("Configuring time...");
    Serial.flush();

    // Wait for time to be set (up to 10 seconds)
    int retries = 0;
    time_t now = time(nullptr);
    while (now < 1000000000 && retries < 20) {
        delay(500);
        now = time(nullptr);
        retries++;
    }
    if (now > 1000000000) {
        struct tm timeinfo;
        if (localtime_r(&now, &timeinfo)) {
            Serial.print("Time synchronized: ");
            Serial.print(asctime(&timeinfo));
        }
    } else {
        Serial.println("Warning: Time synchronization failed, time calculations may be inaccurate");
    }
    Serial.flush();

    bool needsBusSetup = forcePortal || !appConfig.configured;
    if (needsBusSetup) {
        beginOTA();
        runBusSetupServer(CONFIG_PORTAL_TIMEOUT_SECONDS);
        if (!appConfig.configured) {
            showStatusMessage("Bus setup timeout", "Press setup button", "Try again");
            enterDeepSleep(appConfig.sleep_minutes * 60);
            return;
        }
        runOTAWindow(OTA_WINDOW_SECONDS);
    }

    refreshBusData();
    enterDeepSleep(plannedSleepSeconds);
}

void loop() {
    if (isSetupButtonPressed()) {
        unsigned long pressedAt = millis();
        while (isSetupButtonPressed() && millis() - pressedAt < 2500) {
            delay(20);
        }
        if (millis() - pressedAt >= 2500) {
            if (connectWiFiAndMaybeConfigure(true)) {
                beginOTA();
                runBusSetupServer(CONFIG_PORTAL_TIMEOUT_SECONDS);
                runOTAWindow(OTA_WINDOW_SECONDS);
                refreshBusData();
            }
            enterDeepSleep(plannedSleepSeconds);
        }
    }

    if (otaStarted) {
        ArduinoOTA.handle();
    }

    delay(100);
}
