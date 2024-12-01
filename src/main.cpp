#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <heltec-eink-modules.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeMono9pt7b.h>

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

StaticJsonDocument<4096> doc;


void display_phrase(int index) {
    JsonArray phrases = doc["phrases"];
    JsonObject phrase = phrases[index];
    
    DRAW(display) {
        // First line - Korean with U8g2
        u8g2_adapter.setFont(u8g2_font_unifont_t_korean2);
        u8g2_adapter.setCursor(10,20);
        u8g2_adapter.print(phrase["korean"].as<const char*>());

        // Second line - English
        display.setFont(&FreeMono9pt7b);
        display.setTextSize(1);
        display.setCursor(10, 60);
        display.print(phrase["english"].as<const char*>());

        // Third line - Pronunciation
        display.setCursor(10, 100);
        display.print(phrase["pronunciation"].as<const char*>());
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(21, INPUT);
    
    // Initialize LittleFS
    if(!LittleFS.begin()){
        Serial.println("LittleFS Mount Failed");
        return;
    }

    // Read and parse JSON file
    File file = LittleFS.open("/phrases.json", "r");
    if(!file){
        Serial.println("Failed to open file");
        return;
    }

    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println("JSON parsing failed!");
        return;
    }
    file.close();

    display.landscape();
    u8g2_adapter.begin();
    display_phrase(0);  // Show first phrase
}


void loop() {
    static int currentPhrase = 0;
    static int phraseCount = doc["phrases"].size();
    
    if (digitalRead(21) == LOW) {
        currentPhrase = (currentPhrase + 1) % phraseCount;
        display_phrase(currentPhrase);
        delay(200);  // Simple debounce
    }
    delay(100);
}
