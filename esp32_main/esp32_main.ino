#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <vector>

// ====== CONFIGURATION ======
// define your creds before uploading
#define WIFI_SSID "darthmendespi4"
#define WIFI_PASSWORD "12345"
#define HOTSPOT_SSID "LEDlights"
#define HOTSPOT_PASSWORD "darth1"

#define LED_PIN     15
#define NUM_LEDS    60
#define DEFAULT_BRIGHTNESS 255

// ==========================

CRGB leds[NUM_LEDS];
WebServer server(80);

enum Mode {
  MODE_OFF,
  MODE_ON,
  MODE_THUNDERSTORM,
  MODE_GRADIENT,
  MODE_FIRE
};

enum OverlayMode {
  OVERLAY_NONE,
  OVERLAY_PULSE,
  OVERLAY_WAVE,
  OVERLAY_CIRCLING,
  OVERLAY_UPDOWN
};

struct StyleConfig {
  Mode baseMode = MODE_OFF;
  OverlayMode overlayMode = OVERLAY_NONE;
  uint8_t start = 0;
  uint8_t end = NUM_LEDS - 1;
  uint8_t speed = 5;
  uint8_t intensity = 128;
  uint8_t brightness = DEFAULT_BRIGHTNESS;
  uint8_t size = 1;
  bool directionForward = true;

  CRGB flashColor = CRGB::White;
  CRGB baseColor = CRGB::White;
  CRGB overlayColor = CRGB::White;
  std::vector<CRGB> gradientColors;

  // NEW: Segmented Ambient Support
  bool ambientSegmented = false;
  std::vector<CRGB> ambientSegmentColors;
  uint8_t ambientBlend = 0; // for backward compatibility
  CRGB ambientColor = CRGB::Black; // for backward compatibility
};

StyleConfig currentStyle;

// Thunderstorm state
struct Lightning {
  bool active = false;
  unsigned long endTime = 0;
  int flashCount = 0;
  unsigned long nextFlashTime = 0;
  unsigned long flashEndTime = 0;
  int length = 0;
  int start = 0;
  CRGB color;
};
Lightning lightning;
unsigned long lastStormTrigger = 0;

#define FIRE_SPEED 2
#define FIRE_COOLING 5
#define FIRE_SPARKS 2000
unsigned short* fireHeat = nullptr;
unsigned long lastFireUpdate = 0;

// Critical section mutex
portMUX_TYPE styleMux = portMUX_INITIALIZER_UNLOCKED;

// ==========================
// Manual blend function for CRGB
// ==========================
CRGB blendCRGB(const CRGB& color1, const CRGB& color2, uint8_t blendAmount) {
  return CRGB(
    lerp8by8(color1.r, color2.r, blendAmount),
    lerp8by8(color1.g, color2.g, blendAmount),
    lerp8by8(color1.b, color2.b, blendAmount)
  );
}

// ==========================
// Utility
// ==========================
CRGB parseColor(const String& rKey, const String& gKey, const String& bKey, const CRGB& defaultColor = CRGB::White) {
  if (server.hasArg(rKey) && server.hasArg(gKey) && server.hasArg(bKey)) {
    return CRGB(
      constrain(server.arg(rKey).toInt(), 0, 255),
      constrain(server.arg(gKey).toInt(), 0, 255),
      constrain(server.arg(bKey).toInt(), 0, 255)
    );
  }
  return defaultColor;
}

// ==========================
// Parse style
// ==========================
bool parseStyleFromRequest() {
  StyleConfig newStyle;

  // Clean up fire memory when switching away
  if (currentStyle.baseMode == MODE_FIRE && fireHeat) {
    delete[] fireHeat;
    fireHeat = nullptr;
  }

  newStyle.start = constrain(server.hasArg("start") ? server.arg("start").toInt() : 0, 0, NUM_LEDS - 1);
  newStyle.end = constrain(server.hasArg("end") ? server.arg("end").toInt() : NUM_LEDS - 1, 0, NUM_LEDS - 1);
  if (newStyle.start > newStyle.end) std::swap(newStyle.start, newStyle.end);

  newStyle.speed = constrain(server.hasArg("speed") ? server.arg("speed").toInt() : 5, 1, 10);
  newStyle.intensity = constrain(server.hasArg("intensity") ? server.arg("intensity").toInt() : 128, 0, 255);
  newStyle.brightness = constrain(server.hasArg("brightness") ? server.arg("brightness").toInt() : DEFAULT_BRIGHTNESS, 0, 255);
  newStyle.size = constrain(server.hasArg("size") ? server.arg("size").toInt() : 1, 1, 20);
  newStyle.directionForward = !server.hasArg("dir") || server.arg("dir") == "f";

  // Parse segmented ambient
  newStyle.ambientSegmented = false;
  newStyle.ambientSegmentColors.clear();

  if (server.hasArg("ambient_segments")) {
    int numSeg = constrain(server.arg("ambient_segments").toInt(), 1, 10);
    newStyle.ambientSegmented = true;
    for (int i = 0; i < numSeg; i++) {
      CRGB c = parseColor("seg_r" + String(i), "seg_g" + String(i), "seg_b" + String(i));
      newStyle.ambientSegmentColors.push_back(c);
    }
    // Also parse blend amount
    newStyle.ambientBlend = constrain(server.hasArg("ambient_blend") ? server.arg("ambient_blend").toInt() : 128, 0, 255);
  } else {
    // Fallback to legacy single-color ambient
    newStyle.ambientColor = parseColor("ambient_r", "ambient_g", "ambient_b", CRGB::Black);
    newStyle.ambientBlend = constrain(server.hasArg("ambient_blend") ? server.arg("ambient_blend").toInt() : 0, 0, 255);
  }

  // Base mode
  String modeName = server.hasArg("name") ? server.arg("name") : "off";
  if (modeName == "off") newStyle.baseMode = MODE_OFF;
  else if (modeName == "on") newStyle.baseMode = MODE_ON;
  else if (modeName == "thunderstorm") newStyle.baseMode = MODE_THUNDERSTORM;
  else if (modeName == "gradient") newStyle.baseMode = MODE_GRADIENT;
  else if (modeName == "fire") newStyle.baseMode = MODE_FIRE;
  else return false;

  // Colors
  newStyle.gradientColors.clear();
  if (server.hasArg("colors")) {
    int num = constrain(server.arg("colors").toInt(), 1, 8);
    for (int i = 0; i < num; i++) {
      newStyle.gradientColors.push_back(parseColor("r" + String(i), "g" + String(i), "b" + String(i)));
    }
  }
  if (newStyle.gradientColors.empty()) newStyle.gradientColors.push_back(CRGB::White);

  newStyle.baseColor = newStyle.gradientColors[0];
  if (modeName == "thunderstorm") {
    newStyle.flashColor = parseColor("c2r", "c2g", "c2b");
    lightning.color = newStyle.flashColor;
  }

  // Overlay
  newStyle.overlayMode = OVERLAY_NONE;
  if (server.hasArg("overlay")) {
    String overlay = server.arg("overlay");
    if (overlay == "pulse") newStyle.overlayMode = OVERLAY_PULSE;
    else if (overlay == "wave") newStyle.overlayMode = OVERLAY_WAVE;
    else if (overlay == "circling") newStyle.overlayMode = OVERLAY_CIRCLING;
    else if (overlay == "updown") newStyle.overlayMode = OVERLAY_UPDOWN;

    if (newStyle.overlayMode != OVERLAY_NONE) {
      newStyle.overlayColor = parseColor("or", "og", "ob", newStyle.baseColor);
    }
  }

  // Apply new style atomically
  portENTER_CRITICAL(&styleMux);
  currentStyle = newStyle;
  portEXIT_CRITICAL(&styleMux);

  lightning.active = false;
  lastStormTrigger = millis();
  return true;
}

// ==========================
// Render base layer
// ==========================
void renderBase() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  if (currentStyle.baseMode == MODE_OFF) return;

  if (currentStyle.baseMode == MODE_THUNDERSTORM) {
    return;
  }

  if (currentStyle.baseMode == MODE_ON) {
    for (int i = currentStyle.start; i <= currentStyle.end; i++) {
      leds[i] = currentStyle.baseColor;
    }
  } else if (currentStyle.baseMode == MODE_GRADIENT) {
    int range = currentStyle.end - currentStyle.start;
    if (currentStyle.gradientColors.size() == 1) {
      for (int i = currentStyle.start; i <= currentStyle.end; i++) {
        leds[i] = currentStyle.gradientColors[0];
      }
    } else if (currentStyle.gradientColors.size() == 2) {
      for (int i = currentStyle.start; i <= currentStyle.end; i++) {
        uint8_t blendAmount = map(i - currentStyle.start, 0, range, 0, 255);
        leds[i] = blendCRGB(currentStyle.gradientColors[0], currentStyle.gradientColors[1], blendAmount);
      }
    } else {
      for (int i = currentStyle.start; i <= currentStyle.end; i++) {
        float progress = range > 0 ? (float)(i - currentStyle.start) / range : 0;
        int seg = min((int)(progress * (currentStyle.gradientColors.size() - 1)), (int)currentStyle.gradientColors.size() - 2);
        float local = (progress * (currentStyle.gradientColors.size() - 1)) - seg;
        uint8_t blendAmount = (uint8_t)(local * 255);
        leds[i] = blendCRGB(currentStyle.gradientColors[seg], currentStyle.gradientColors[seg + 1], blendAmount);
      }
    }
  }
}

// ==========================
// RENDER FIRE
// ==========================
void renderFire() {
  if (!fireHeat) {
    fireHeat = new (std::nothrow) unsigned short[NUM_LEDS + FIRE_SPEED + 3];
    if (!fireHeat) {
      // Out of memory - show red error
      fill_solid(leds, NUM_LEDS, CRGB::Red);
      return;
    }
    for (int i = 0; i < NUM_LEDS + FIRE_SPEED + 3; i++) {
      fireHeat[i] = 0;
    }
  }

  unsigned long now = millis();
  if (now - lastFireUpdate >= 10) {
    lastFireUpdate = now;

    // Add sparks at the base
    for (int i = 0; i < FIRE_SPEED; i++) {
      fireHeat[NUM_LEDS + i] = random(random(random(FIRE_SPARKS)));
    }

    // Propagate heat + cooling
    bool anyHeat = false;
    for (int i = 0; i < NUM_LEDS; i++) {
      int h = (fireHeat[i + FIRE_SPEED - 1] * 3 +
               fireHeat[i + FIRE_SPEED] * 10 +
               fireHeat[i + FIRE_SPEED + 1] * 3) >> 4;
      h = h - random(FIRE_COOLING);
      if (h < 0) h = 0;
      fireHeat[i] = h;
      if (h > 0) anyHeat = true;
    }

    if (!anyHeat) {
      for (int i = 0; i < NUM_LEDS + FIRE_SPEED + 3; i++) fireHeat[i] = 0;
    }
  }

  // Color mapping
  CRGB color1 = currentStyle.gradientColors.size() > 0 ? currentStyle.gradientColors[0] : CRGB(255, 60, 0);
  CRGB color2 = currentStyle.gradientColors.size() > 1 ? currentStyle.gradientColors[1] : CRGB(255, 150, 0);

  for (int i = 0; i < NUM_LEDS; i++) {
    int heat = fireHeat[NUM_LEDS - 1 - i];
    CRGB c;
    if (heat < 256) {
      c = color1;
      c.nscale8_video(heat);
    } else if (heat < 512) {
      uint8_t blend = heat - 256;
      c = blendCRGB(color1, color2, blend);
    } else if (heat < 768) {
      uint8_t blend = heat - 512;
      c = blendCRGB(color2, CRGB::White, blend);
    } else {
      c = CRGB::White;
      c.nscale8_video(255);
    }
    leds[i] = c;
  }

  // Apply active range
  for (int i = 0; i < currentStyle.start; i++) leds[i] = CRGB::Black;
  for (int i = currentStyle.end + 1; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
}

// ==========================
// Render overlay
// ==========================
void renderOverlay() {
  if (currentStyle.overlayMode == OVERLAY_NONE) return;

  unsigned long now = millis();
  float t = now * currentStyle.speed * 0.001;

  if (currentStyle.overlayMode == OVERLAY_PULSE) {
    uint8_t pulse = sin8(t * 128) * currentStyle.intensity / 255;
    CRGB c = currentStyle.overlayColor;
    c.nscale8_video(pulse);
    for (int i = currentStyle.start; i <= currentStyle.end; i++) {
      leds[i] = c;
    }
  } else if (currentStyle.overlayMode == OVERLAY_WAVE) {
    for (int i = currentStyle.start; i <= currentStyle.end; i++) {
      int offset = currentStyle.directionForward ? (i - currentStyle.start) : (currentStyle.end - i);
      uint8_t wave = sin8(offset * 2 + t * 64) * currentStyle.intensity / 255;
      CRGB c = currentStyle.overlayColor;
      c.nscale8_video(wave);
      leds[i] = c;
    }
  } else if (currentStyle.overlayMode == OVERLAY_CIRCLING) {
    int range = currentStyle.end - currentStyle.start + 1;
    int pos = currentStyle.directionForward 
      ? (currentStyle.start + ((int)(t * currentStyle.speed) % range))
      : (currentStyle.end - ((int)(t * currentStyle.speed) % range));
    for (int s = 0; s < currentStyle.size && pos + s <= currentStyle.end; s++) {
      if (pos + s >= currentStyle.start) {
        leds[pos + s] = currentStyle.overlayColor;
      }
    }
  } else if (currentStyle.overlayMode == OVERLAY_UPDOWN) {
    int range = currentStyle.end - currentStyle.start + 1;
    int step = ((int)(t * currentStyle.speed * 2)) % (range * 2);
    if (step < range) {
      for (int i = 0; i <= step; i++) {
        if (currentStyle.start + i <= currentStyle.end) {
          leds[currentStyle.start + i] = currentStyle.overlayColor;
        }
      }
    } else {
      for (int i = range - 1; i >= (range * 2 - 1 - step); i--) {
        if (i >= 0 && currentStyle.start + i <= currentStyle.end) {
          leds[currentStyle.start + i] = currentStyle.overlayColor;
        }
      }
    }
  }
}

// ==========================
// Thunderstorm (full control)
// ==========================
void renderThunderstorm() {
  if (currentStyle.gradientColors.size() == 1) {
    for (int i = currentStyle.start; i <= currentStyle.end; i++) {
      CRGB c = currentStyle.gradientColors[0];
      c.nscale8_video(currentStyle.intensity);
      leds[i] = c;
    }
  } else {
    int range = currentStyle.end - currentStyle.start;
    for (int i = currentStyle.start; i <= currentStyle.end; i++) {
      float progress = range > 0 ? (float)(i - currentStyle.start) / range : 0;
      int seg = min((int)(progress * (currentStyle.gradientColors.size() - 1)), (int)currentStyle.gradientColors.size() - 2);
      float local = (progress * (currentStyle.gradientColors.size() - 1)) - seg;
      uint8_t blendAmount = (uint8_t)(local * 255);
      CRGB c = blendCRGB(currentStyle.gradientColors[seg], currentStyle.gradientColors[seg + 1], blendAmount);
      c.nscale8_video(currentStyle.intensity);
      leds[i] = c;
    }
  }

  for (int i = 0; i < currentStyle.start; i++) leds[i] = CRGB::Black;
  for (int i = currentStyle.end + 1; i < NUM_LEDS; i++) leds[i] = CRGB::Black;

  unsigned long now = millis();

  if (lightning.active) {

    if (now >= lightning.endTime) {
      lightning.active = false;
      lastStormTrigger = now; 
      return;
    }

    // Handle flash sequencing
    if (now >= lightning.nextFlashTime) {
      if (currentStyle.gradientColors.size() == 1) {
          for (int i = currentStyle.start; i <= currentStyle.end; i++) {
            CRGB c = currentStyle.gradientColors[0];
            c.nscale8_video(currentStyle.intensity);
            leds[i] = c;
          }
        } else {
          int range = currentStyle.end - currentStyle.start;
          for (int i = currentStyle.start; i <= currentStyle.end; i++) {
            float progress = range > 0 ? (float)(i - currentStyle.start) / range : 0;
            int seg = min((int)(progress * (currentStyle.gradientColors.size() - 1)), (int)currentStyle.gradientColors.size() - 2);
            float local = (progress * (currentStyle.gradientColors.size() - 1)) - seg;
            uint8_t blendAmount = (uint8_t)(local * 255);
            CRGB c = blendCRGB(currentStyle.gradientColors[seg], currentStyle.gradientColors[seg + 1], blendAmount);
            c.nscale8_video(currentStyle.intensity);
            leds[i] = c;
          }
        }
      // Start a new flash in the sequence
      if (now < lightning.flashEndTime) {
        int flashEnd = min(lightning.start + lightning.length, NUM_LEDS);
        for (int i = lightning.start; i < flashEnd; i++) {
          leds[i] = lightning.color;
        }
      }
      int flashDuration = (lightning.flashCount == 0) ? random(30, 50) : random(10, 25);
      lightning.flashEndTime = now + flashDuration;

      lightning.flashCount++;

      if (lightning.flashCount < random(2, 5)) {
        lightning.nextFlashTime = now + random(30, 100); // gap after this flash
      } else {
        lightning.endTime = now + flashDuration; // end after this flash
      }
    }
    //return;
  }
    // Trigger new storm
  uint16_t minInterval = 5000 / (currentStyle.speed + 1);
  uint16_t maxInterval = 15000 / (currentStyle.speed + 1);
  Serial.print("Flash update: ");
  Serial.println(lastStormTrigger);
  Serial.print("Flash is active: ");
  Serial.println(lightning.active);
  if (now - lastStormTrigger > random(minInterval, maxInterval)) {
    lightning.active = true;
    lightning.flashCount = 0;
    lightning.color = currentStyle.flashColor;
    lightning.length = random(5, 20);
    lightning.start = random(0, NUM_LEDS - lightning.length);
    lightning.nextFlashTime = now + (random(100,200)/currentStyle.speed+1); // first flash NOW
    lightning.endTime = lightning.nextFlashTime + 800; // safety timeout
    lastStormTrigger = now;
  }
}

// ==========================
// Main render with segmented ambient blending
// ==========================
void render() {
  // Enter critical section to read currentStyle safely
  StyleConfig localStyle;
  portENTER_CRITICAL(&styleMux);
  localStyle = currentStyle;
  portEXIT_CRITICAL(&styleMux);
  
  // EXCLUSIVE AMBIENT MODE: If segmented ambient is active, render ONLY that
  if (localStyle.ambientSegmented && !localStyle.ambientSegmentColors.empty()) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    
    int numSegments = localStyle.ambientSegmentColors.size();
    int ledRange = localStyle.end - localStyle.start + 1;
    int segSize = ledRange / numSegments;

    for (int seg = 0; seg < numSegments; seg++) {
      // 🔁 FLIP: Screen top (seg=0) → LED top (higher indices)
      int screenSegIndex = seg;
      int ledSegIndex = numSegments - 1 - seg;

      CRGB ambientColor = localStyle.ambientSegmentColors[screenSegIndex];
      int segStart = localStyle.start + ledSegIndex * segSize;
      int segEnd = (ledSegIndex == numSegments - 1) ? localStyle.end : segStart + segSize - 1;

      for (int i = segStart; i <= segEnd; i++) {
        if (i >= 0 && i < NUM_LEDS) {
          leds[i] = ambientColor; // NO BLENDING — pure ambient color
        }
      }
    }
  }
  // Legacy single-color ambient (for backward compatibility)
  else if (localStyle.ambientBlend > 0) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int i = localStyle.start; i <= localStyle.end; i++) {
      if (i >= 0 && i < NUM_LEDS) {
        leds[i] = localStyle.ambientColor; // Pure color, no blend
      }
    }
  }
  // Normal mode: render base + overlay + effects
  else {
    if (localStyle.baseMode == MODE_THUNDERSTORM) {
      renderThunderstorm();
    } else if (localStyle.baseMode == MODE_FIRE) {
      renderFire();
    } else {
      renderBase();
      renderOverlay();
      for (int i = 0; i < localStyle.start; i++) leds[i] = CRGB::Black;
      for (int i = localStyle.end + 1; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
    }
  }
  FastLED.setBrightness(localStyle.brightness);
  FastLED.show();
}

// ==========================
// Handlers
// ==========================
void handleMode() {
  if (parseStyleFromRequest()) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "ERROR: Invalid mode");
  }
}

void handleTrigger() {
  // Enter critical section to read current style safely
  CRGB flashColor;
  portENTER_CRITICAL(&styleMux);
  flashColor = currentStyle.flashColor;
  portEXIT_CRITICAL(&styleMux);

  lightning.active = false;
  lightning.flashCount = 0;
  lightning.color = flashColor;
  lightning.length = random(5, NUM_LEDS);
  lightning.start = random(0, NUM_LEDS - lightning.length);
  lightning.nextFlashTime = millis();
  lightning.flashEndTime = millis() + random(90, 130);
  lightning.endTime = millis() + 800;
  lightning.active = true;
  lastStormTrigger = millis();
  server.send(200, "text/plain", "OK");
}

// ==========================
// Setup & Loop
// ==========================
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  Serial.println("Attempting connection to network...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait max 10 seconds for the Wi-Fi network
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 20) {
    delay(500);
    Serial.print(".");
    counter++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Connect to Wifi
    Serial.println("Connected: " + WiFi.localIP().toString());
  } else {
    // Creates Hotspot and works alone
    Serial.println("\nNetwork not found. Switching to Single Mode...");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(HOTSPOT_SSID, HOTSPOT_PASSWORD); 
    Serial.print("Hotspot launched! Connect to: ");
    Serial.println(HOTSPOT_SSID);
    Serial.print("Control URL: http://");
    Serial.println(WiFi.softAPIP()); // Defaults to 192.168.4.1
  }

  server.on("/mode", HTTP_GET, handleMode);
  server.on("/trigger", HTTP_GET, handleTrigger);
  server.begin();
}

void loop() {
  server.handleClient();
  render();
  // No delay — let WiFi and rendering run freely
}