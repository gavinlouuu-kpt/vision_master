#include <Arduino.h>
#include <heltec-eink-modules.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeMono9pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
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
const char* ROUTE_NUMBER = "299X";
const char* STOP_ID = "ST142";  // Fallback stop_id if dynamic stop selection fails
const char* DEFAULT_BOUND = "O";  // O for outbound, I for inbound
const int DEFAULT_SERVICE_TYPE = 1;
const int DEFAULT_STOP_SEQ = 3;   // Select the 3rd stop from route-stop list
unsigned long lastFetchTime = 0;
const unsigned long FETCH_INTERVAL = 60000; // 60 seconds (1 minute) - reduced for power savings

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

// Array to hold multiple ETAs (next 3 buses)
const int MAX_ETAS = 3;
ETAInfo etaList[MAX_ETAS];
int etaCount = 0;

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

// Forward declaration
bool fetchRouteInfo(const char* route, const char* bound, int service_type);
bool fetchStopInfo(const char* stopId);
void displayStopInfoSerial(const StopInfo& info);
bool selectStopIdBySeq(const char* route, const char* bound, int service_type, int seq, String& outStopId);
bool listRouteStops(const char* route, const char* bound, int service_type);
bool fetchRouteETAInfo(const char* route, int service_type, int targetSeq, const char* targetBound);
int calculateMinutesUntilETA(const String& etaStr);

// Function to find valid route combinations from Route List API
bool findValidRouteCombinations(const char* route, String& foundBound, int& foundServiceType) {
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

// Function to fetch stop (station) information from API
bool fetchStopInfo(const char* stopId) {
    Serial.println("\n=== Fetching Stop Information ===");
    Serial.flush();

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
            currentStopInfo.valid = false;
            return false;
        }

        if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
            JsonObject data = doc["data"].as<JsonObject>();

            currentStopInfo.stop_id = data["stop"].as<String>();
            currentStopInfo.name_en = data["name_en"].as<String>();
            currentStopInfo.name_tc = data["name_tc"].as<String>();
            currentStopInfo.lat = data["lat"].as<float>();
            currentStopInfo.lon = data["long"].as<float>();
            currentStopInfo.valid = true;

            Serial.println("Successfully loaded stop info");
            displayStopInfoSerial(currentStopInfo);

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
        currentStopInfo.valid = false;
        return false;
    } else if (httpCode < 0) {
        Serial.print("Connection error: ");
        Serial.println(httpCode);
        Serial.println("Error details: " + http.errorToString(httpCode));
        Serial.flush();
        http.end();
        currentStopInfo.valid = false;
        return false;
    } else {
        Serial.print("HTTP request failed, code: ");
        Serial.println(httpCode);
        String errorPayload = http.getString();
        Serial.println("Error response: " + errorPayload);
        Serial.flush();
        http.end();
        currentStopInfo.valid = false;
        return false;
    }
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
            
            // Update last fetch time when ETAs are successfully loaded
            if (etaCount > 0) {
                lastETAFetchTime = millis();
            }
            
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
    Serial.print(ROUTE_NUMBER);
    Serial.print(" | Stop: ");
    if (currentStopInfo.valid && currentStopInfo.name_en.length() > 0) {
        Serial.print(currentStopInfo.name_en);
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
    // Get current time
    time_t now = time(nullptr);
    
    time_t targetTime;
    if (fetchTime == 0) {
        // If no fetch time, use current time
        targetTime = now;
    } else {
        // Calculate the actual time when fetch happened
        // fetchTime is in millis(), convert to seconds and subtract from current time
        unsigned long secondsAgo = (millis() - fetchTime) / 1000;
        targetTime = now - secondsAgo;
    }
    
    if (now < 1000000000) {
        // Time not synchronized, show relative time
        if (fetchTime == 0) {
            return "Now";
        }
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
        if (fetchTime == 0) {
            return "Now";
        }
        unsigned long secondsAgo = (millis() - fetchTime) / 1000;
        if (secondsAgo < 60) {
            return String(secondsAgo) + "s ago";
        } else if (secondsAgo < 3600) {
            return String(secondsAgo / 60) + "m ago";
        } else {
            return String(secondsAgo / 3600) + "h ago";
        }
    }
    
    // Format as "HH:MM:SS" (GMT+8 timezone)
    char timeStr[10];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", 
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(timeStr);
}

// Function to display ETA info on e-ink display
void displayETAEInk() {
    display.clearMemory();
    
    // Use smaller font for header and other text (8pt)
    u8g2_adapter.setFont(u8g2_font_helvR08_tf);
    
    int yPos = 10;
    int lineHeight = 12;
    
    // Display header - Route and stop name
    u8g2_adapter.setCursor(5, yPos);
    String stopLabel = selectedStopId;
    if (currentStopInfo.valid && currentStopInfo.name_en.length() > 0) {
        stopLabel = currentStopInfo.name_en;
        // Truncate stop name if too long
        if (stopLabel.length() > 25) {
            stopLabel = stopLabel.substring(0, 22) + "...";
        }
    }
    String header = String(ROUTE_NUMBER) + " @ " + stopLabel;
    // Truncate if too long for the display
    if (header.length() > 28) {
        header = header.substring(0, 25) + "...";
    }
    u8g2_adapter.print(header);
    yPos += lineHeight + 2;
    
    // Display destination (once, not repeated for each bus)
    u8g2_adapter.setCursor(5, yPos);
    String dest = "-> " + (etaCount > 0 ? etaList[0].dest_en : "SAI KUNG");
    if (dest.length() > 28) {
        dest = dest.substring(0, 25) + "...";
    }
    u8g2_adapter.print(dest);
    yPos += lineHeight + 2;
    
    // Display updated time above all ETAs (always show content)
    u8g2_adapter.setCursor(5, yPos);
    // Use 12pt font for timestamp
    u8g2_adapter.setFont(u8g2_font_helvR12_tf);
    String updated = "Updated: " + formatLastUpdated(lastETAFetchTime);
    if (updated.length() > 28) {
        updated = updated.substring(0, 25) + "...";
    }
    u8g2_adapter.print(updated);
    yPos += 16 + 2; // Larger line height for 12pt font
    
    if (etaCount == 0) {
        u8g2_adapter.setCursor(5, yPos);
        u8g2_adapter.print("No ETAs available");
    } else {
        // Display next 3 buses - all in 12pt font
        u8g2_adapter.setFont(u8g2_font_helvR12_tf);
        int busLineHeight = 16; // Larger line height for 12pt font
        
        for (int i = 0; i < etaCount && i < MAX_ETAS; i++) {
            // Bus number and time - all in 12pt font
            u8g2_adapter.setCursor(5, yPos);
            String busInfo = "#" + String(i + 1) + "  " + formatETA(etaList[i].eta);
            // Truncate if too long
            if (busInfo.length() > 18) {
                busInfo = busInfo.substring(0, 15) + "...";
            }
            u8g2_adapter.print(busInfo);
            yPos += busLineHeight + 2;
            
            // Skip remark line - no need to show "Scheduled Bus" text
        }
    }
    
    // Display last updated time at bottom
    if (lastETAFetchTime > 0) {
        yPos = display.height() - 8;
        u8g2_adapter.setCursor(5, yPos);
        // Use smaller font for timestamp
        u8g2_adapter.setFont(u8g2_font_helvR08_tf);
        String updated = "Updated at " + formatLastUpdated(lastETAFetchTime);
        if (updated.length() > 28) {
            updated = updated.substring(0, 25) + "...";
        }
        u8g2_adapter.print(updated);
    }
    
    display.update();
}

void setup() {
    Serial.begin(115200);
    delay(2000); // Longer delay to ensure Serial is ready
    
    // Wait for Serial to be available (for USB CDC)
    while (!Serial && millis() < 5000) {
        delay(10);
    }
    
    Serial.println("\n\n========================================");
    Serial.println("KMB Route 299X ETA Display");
    Serial.println("========================================");
    Serial.println("Initializing...");
    Serial.flush();
    
    pinMode(21, INPUT);
    
    // Initialize e-ink display
    display.landscape();
    u8g2_adapter.begin();
    
    // Display initial message
    display.clearMemory();
    u8g2_adapter.setFont(u8g2_font_helvR08_tf);
    u8g2_adapter.setCursor(5, 20);
    u8g2_adapter.print("Connecting WiFi...");
    display.update();
    
    // Initialize WiFiManager
    WiFiManager wifiManager;
    
    // Uncomment to reset WiFi settings (for testing)
    // wifiManager.resetSettings();
    
    // Set custom AP name
    wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    
    // Attempt to connect to saved WiFi, or start configuration portal
    if (!wifiManager.autoConnect("KMB-Route-299-AP")) {
        Serial.println("Failed to connect to WiFi and hit timeout");
        display.clearMemory();
        u8g2_adapter.setCursor(5, 20);
        u8g2_adapter.print("WiFi Failed!");
        display.update();
        delay(3000);
        ESP.restart();
    }
    
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
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
    
    // Display WiFi connected message
    display.clearMemory();
    u8g2_adapter.setCursor(5, 20);
    u8g2_adapter.print("WiFi Connected");
    u8g2_adapter.setCursor(5, 35);
    u8g2_adapter.print("Fetching ETAs...");
    display.update();
    
    // Initialize structures
    currentRouteInfo.valid = false;
    currentStopInfo.valid = false;
    etaCount = 0;
    
    // Try to fetch route info first to get service_type, but don't fail if it doesn't work
    Serial.println("\nAttempting to fetch route information...");
    Serial.flush();
    fetchRouteInfoWithRetry(ROUTE_NUMBER);

    // Determine service type to use
    int serviceType = currentRouteInfo.valid ? currentRouteInfo.service_type : DEFAULT_SERVICE_TYPE;
    const char* boundToUse = currentRouteInfo.valid ? currentRouteInfo.bound.c_str() : DEFAULT_BOUND;

    // List all stops for the route first
    Serial.println("\n=== Listing All Stops for Route ===");
    Serial.flush();
    listRouteStops(ROUTE_NUMBER, boundToUse, serviceType);
    Serial.flush();
    delay(1000); // Give time to read the output

    // Select the 2nd stop (seq=2) from route-stop list; fall back to configured STOP_ID if needed
    String stopFromSeq;
    if (selectStopIdBySeq(ROUTE_NUMBER, boundToUse, serviceType, selectedStopSeq, stopFromSeq)) {
        selectedStopId = stopFromSeq;
    } else {
        selectedStopId = String(STOP_ID);
    }

    // Fetch stop (station) info so we can display a friendly name
    Serial.println("\nAttempting to fetch stop information...");
    Serial.flush();
    fetchStopInfo(selectedStopId.c_str());
    
    // Fetch ETA information
    Serial.println("\nAttempting to fetch ETA information...");
    Serial.flush();
    
    // Try Route ETA API first (recommended - more reliable)
    // Then fallback to ETA API if needed
    bool etaSuccess = false;
    
    // Primary: Route ETA API (uses seq instead of stop_id)
    if (fetchRouteETAInfo(ROUTE_NUMBER, serviceType, selectedStopSeq, boundToUse)) {
        etaSuccess = true;
    } else if (fetchRouteETAInfo(ROUTE_NUMBER, DEFAULT_SERVICE_TYPE, selectedStopSeq, boundToUse)) {
        etaSuccess = true;
    } else if (fetchRouteETAInfo(ROUTE_NUMBER, 2, selectedStopSeq, boundToUse)) {
        etaSuccess = true;
    }
    // Fallback: ETA API (original method)
    else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), serviceType)) {
        etaSuccess = true;
    } else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), DEFAULT_SERVICE_TYPE)) {
        etaSuccess = true;
    } else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), 2)) {
        etaSuccess = true;
    }
    
    if (etaSuccess) {
        Serial.println("Successfully fetched ETA info!");
        displayETASerial();
        displayETAEInk();
    } else {
        Serial.println("ERROR: Failed to fetch ETA information");
        Serial.println("Check WiFi connection and API availability");
        Serial.flush();
        
        display.clearMemory();
        u8g2_adapter.setFont(u8g2_font_helvR08_tf);
        u8g2_adapter.setCursor(5, 20);
        u8g2_adapter.print("ETA API Error");
        u8g2_adapter.setCursor(5, 35);
        u8g2_adapter.print("Check Serial");
        display.update();
    }
}

void loop() {
    unsigned long currentTime = millis();
    
    // Check if it's time to fetch ETA info again
    if (currentTime - lastFetchTime >= FETCH_INTERVAL) {
        lastFetchTime = currentTime;
        
        Serial.println("\n=== Periodic Update ===");
        Serial.println("Fetching updated ETA information...");
        Serial.flush();
        
        // Determine service type to use
        int serviceType = currentRouteInfo.valid ? currentRouteInfo.service_type : DEFAULT_SERVICE_TYPE;
        const char* boundToUse = currentRouteInfo.valid ? currentRouteInfo.bound.c_str() : DEFAULT_BOUND;
        
        // Try Route ETA API first (recommended - more reliable)
        // Then fallback to ETA API if needed
        bool etaSuccess = false;
        if (fetchRouteETAInfo(ROUTE_NUMBER, serviceType, selectedStopSeq, boundToUse)) {
            etaSuccess = true;
        } else if (fetchRouteETAInfo(ROUTE_NUMBER, DEFAULT_SERVICE_TYPE, selectedStopSeq, boundToUse)) {
            etaSuccess = true;
        } else if (fetchRouteETAInfo(ROUTE_NUMBER, 2, selectedStopSeq, boundToUse)) {
            etaSuccess = true;
        }
        // Fallback: ETA API (original method)
        else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), serviceType)) {
            etaSuccess = true;
        } else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), DEFAULT_SERVICE_TYPE)) {
            etaSuccess = true;
        } else if (fetchETAInfo(ROUTE_NUMBER, selectedStopId.c_str(), 2)) {
            etaSuccess = true;
        }
        
        if (etaSuccess) {
            displayETASerial();
            displayETAEInk();
        } else {
            Serial.println("ERROR: Failed to fetch ETA information");
            Serial.flush();
            // Keep displaying last known ETAs if available
            if (etaCount > 0) {
                Serial.println("Keeping last known ETA info on display");
                Serial.flush();
                displayETAEInk();
            } else {
                display.clearMemory();
                u8g2_adapter.setFont(u8g2_font_helvR08_tf);
                u8g2_adapter.setCursor(5, 20);
                u8g2_adapter.print("ETA API Error");
                display.update();
            }
        }
    }
    
    delay(1000); // Small delay to prevent excessive loop execution
}
