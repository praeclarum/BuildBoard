#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_GFX.h>
#include <FastLED_NeoMatrix.h>
#include <FastLED.h>
#include <esp_log.h>
#include <ArduinoJson.h>
#include <vector>
#include <fauxmoESP.h>

#include "keys.h"

using namespace std;

#define ALEXA_ID            "Board"

#define UPDATE_APPS_MILLIS  (5*60*1000)

#define MAX_BRIGHTNESS 64
#define MIN_BRIGHTNESS 4

#define SERIAL_BAUDRATE     115200

bool isOn = true;
uint8_t brightness = MAX_BRIGHTNESS;

class App
{
public:
  String title;
  String slug;
  int buildStatus;
  String buildStatusText;
  String buildCommitMessage;
  App()
    : buildStatus(0)
  {
  }
};

fauxmoESP fauxmo;

vector<App> apps;

WiFiMulti WiFiMulti;

#define MATRIX_PIN    12
#define MATRIX_WIDTH  32
#define MATRIX_HEIGHT 8
#define NUMMATRIX     (MATRIX_WIDTH*MATRIX_HEIGHT)

CRGB matrixleds[NUMMATRIX];
FastLED_NeoMatrix matrix = FastLED_NeoMatrix(
  matrixleds,
  MATRIX_WIDTH, MATRIX_HEIGHT,
  //MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);

inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  if (!isOn)
    return 0;
  uint16_t sr = (r * brightness) / 256;
  uint16_t sg = (g * brightness) / 256;
  uint16_t sb = (b * brightness) / 256;
  return ((sr >> 3) << 11) | ((sg >> 2) << 5) | ((sb >> 3) << 0);
}

void setClock() {
  displayStatus("Clock");
  
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print(F("Waiting for NTP time sync: "));
  time_t nowSecs = time(nullptr);
  while (nowSecs < 8 * 3600 * 2) {
    delay(500);
    Serial.print(F("."));
    yield();
    nowSecs = time(nullptr);
  }
  Serial.println();
  struct tm timeinfo;
  gmtime_r(&nowSecs, &timeinfo);
  Serial.print(F("Current time: "));
  Serial.print(asctime(&timeinfo));
}

String get(const String &path)
{
  Serial.printf("GET %s\n", path.c_str());
  WiFiClientSecure client;
  HTTPClient https;  
  auto url = "https://api.bitrise.io/v0.1" + path;
  if (https.begin(client, url)) {  // HTTPS
    https.addHeader("Authorization", BITRISE_TOKEN);
    https.addHeader("accept", "application/json");
    int httpCode = https.GET();
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        return https.getString();        
      }
    } else {
      Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.printf("[HTTPS] Unable to connect\n");
  }
  return "";
}

void displayStatus(const String &message)
{
  matrix.setTextWrap(false);  // we don't wrap text so it scrolls nicely
  matrix.setTextSize(1);
  matrix.setRotation(0);

  uint16_t color = rgb(128, 128, 128);
  matrix.clear();

  drawProgressBar(&matrix);
  
  matrix.setTextColor(color);
  matrix.setCursor(0,0);
  matrix.print(message);
  matrix.show();
}

void getApps()
{
  displayStatus("Apps");

  String basePath = "/users/" BITRISE_USER "/apps?sort_by=last_build_at&limit=10";
  String nextPath = basePath;
  
//  Serial.println(json);

  DynamicJsonDocument doc(10000);

  while (nextPath.length() > 0) {
    Serial.printf("Reading %s...\n", nextPath.c_str());
    auto json = get(nextPath);
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
      Serial.print(F("getApps deserializeJson() failed: "));
      Serial.println(error.c_str());
      return;
    }
    
    const char *title = doc["data"][0]["title"];
    auto i = 0;
    while (title) {
      App app;
      app.title = title;
      app.slug = (const char *)doc["data"][i]["slug"];

      bool needsAdd = true;
      for (const auto &a : apps) {
        if (a.title == app.title) {
          needsAdd = false;
          break;
        }
      }
      if (needsAdd) {
        apps.push_back(app);
      }
      Serial.printf("%s = %s\n", app.title.c_str(), app.slug.c_str());
      
      i++;
      title = doc["data"][i]["title"];
    }
    nextPath = (const char*)doc["paging"]["next"];
    if (nextPath.length() > 0) {
      nextPath = basePath + "&next=" + nextPath;
    }
  }
}

void updateApp(App &app)
{
  auto json = get("/apps/" + app.slug + "/builds?sort_by=created_at&branch=master&limit=1");
//  Serial.println(json);

  DynamicJsonDocument doc(10000);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.print(F("updateApp deserializeJson() failed: "));
    Serial.println(error.c_str());
    return;
  }
  
  const char* sensor = doc["data"][0]["status_text"];
  if (sensor) {
    app.buildStatus = doc["data"][0]["status"];
    app.buildStatusText = app.buildStatus == 1 ? "OK" : sensor;
    app.buildCommitMessage = (const char *)doc["data"][0]["commit_message"];
    Serial.printf("STATUS: %s %s\n", app.title.c_str(), sensor);
  }
}

const int backgroundCanvasScale = 1;
GFXcanvas16 backgroundCanvas(MATRIX_WIDTH*backgroundCanvasScale, MATRIX_HEIGHT*backgroundCanvasScale);
GFXcanvas16 backgroundCanvasScaled(MATRIX_WIDTH, MATRIX_HEIGHT);

void scaleBackground()
{
  auto sb = backgroundCanvas.getBuffer();
  auto sw = backgroundCanvas.width();
  auto sh = backgroundCanvas.height();
//  auto db = backgroundCanvasScaled.getBuffer();
  auto dw = backgroundCanvasScaled.width();
  auto dh = backgroundCanvasScaled.height();

  const int s = backgroundCanvasScale;

  backgroundCanvasScaled.startWrite();
  for (int16_t y=0; y<dh; y++) {
    for (int16_t x=0; x<dw; x++) {
      // x + y * WIDTH
      uint16_t red = 0;
      uint16_t green = 0;
      uint16_t blue = 0;
      for (int16_t j=0; j<s; j++) {
        for (int16_t i=0; i<s; i++) {
          auto color = sb[(s*x+i) + (s*y+j)*sw];
          red += (color >> 11);
          green += (color >> 5) & 0x3F;
          blue += color & 0x1F;
        }
      }
      auto d = s*s;
      red /= d;
      green /= d;
      blue /= d;
      auto color = (red << 11) | (green << 5) | (blue);
      backgroundCanvasScaled.writePixel(x, y, color);
    }
  }
  backgroundCanvasScaled.endWrite();
}

void drawProgressBar(Adafruit_GFX *g)
{
  int numApps = 0;
  int numGoodApps = 0;
  for (const auto &a : apps) {
    if (a.buildStatus == 1)
      numGoodApps++;
    if (a.buildStatus >= 1)
      numApps++;
  }
  if (numApps > 0) {
    float gr = (float)numGoodApps/(float)numApps;
    int w = (int)(gr * MATRIX_WIDTH + 0.5f);
    g->fillRect(w, MATRIX_HEIGHT - 1, MATRIX_WIDTH-w, 1, rgb(32,  0, 0));
    g->fillRect(0, MATRIX_HEIGHT - 1, w,              1, rgb( 0, 32, 0));
  }
}

void drawAppScreen(App &app, const String &message, uint16_t color, int x, GFXcanvas16 *g)
{
//  if (app.buildStatus == 1)
//    g->fillScreen(rgb(0, 24, 0));
//  else
//    g->fillScreen(rgb(32, 0, 0));
//  g->fillScreen(rgb(32, 32, 32));
  g->fillScreen(rgb(0, 0, 0));
    
  drawProgressBar(g);
  
  g->setTextColor(color);
  g->setCursor(x,0);
  g->print(message);
}

void displayApp(App &app)
{
  String message = app.title + " " + app.buildStatusText;

  auto *g = &backgroundCanvas;
  
//  matrix.clear();
  g->setTextWrap(false);  // we don't wrap text so it scrolls nicely
  g->setTextSize(1);
  g->setRotation(0);

//  g->fillRect(           0,             0, MATRIX_WIDTH, MATRIX_HEIGHT, rgb(128, 128,   0));
//  g->fillRect(           0, MATRIX_HEIGHT, MATRIX_WIDTH, MATRIX_HEIGHT, rgb(128, 128, 128));
//  g->fillRect(MATRIX_WIDTH,             0, MATRIX_WIDTH, MATRIX_HEIGHT, rgb(  0, 128, 128));
//  g->fillRect(MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_WIDTH, MATRIX_HEIGHT, rgb(128,   0, 128));

  uint16_t color = 0;
  if (app.buildStatus == 1) {
    color = rgb(0, 255, 0);
  }
  else {
    if (app.buildStatusText.length() > 0) {
      color = rgb(255, 0, 0);
    }
    else {
      color = rgb(255, 255, 0);
    }

    size_t endMessageIndex = 0;
    while (endMessageIndex < app.buildCommitMessage.length()) {
      if (app.buildCommitMessage[endMessageIndex] == '\n')
        break;
      endMessageIndex++;
    }    
    message += ": " + app.buildCommitMessage.substring(0, endMessageIndex);
  }
  int width = (int)(message.length() * 6);
  for (int x=MATRIX_WIDTH; x>=-(width - MATRIX_WIDTH) && isOn; x--) {
    drawAppScreen(app, message, color, x, g);

    scaleBackground();
    matrix.drawRGBBitmap(0, 0, backgroundCanvasScaled.getBuffer(), backgroundCanvasScaled.width(), backgroundCanvasScaled.height());
    matrix.show();

    delay(37);
  }

  if (!isOn) {
    matrix.clear();
    matrix.show();
  }
}

void setupAlexa()
{
  displayStatus("Alexa");

  fauxmo.createServer(true); // not needed, this is the default value
  fauxmo.setPort(80); // This is required for gen3 devices
  fauxmo.enable(true);
  fauxmo.addDevice(ALEXA_ID);
  fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
    Serial.printf("[ALEXA] Device #%d (%s) state: %s value: %d\n", device_id, device_name, state ? "ON" : "OFF", value);
    if (strcmp(device_name, ALEXA_ID)==0) {
      brightness = (int)(((float)value * MAX_BRIGHTNESS) / 255.0f + 0.5f);
      isOn = state;
      if (isOn && brightness < MIN_BRIGHTNESS) {
        brightness = MIN_BRIGHTNESS;
      }
      Serial.printf("IsOn = %d, Brightness = %d", isOn?1:0, brightness);
    }
  });
  fauxmo.setState(ALEXA_ID, true, (255 * brightness) / MAX_BRIGHTNESS);
}

#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif

void setup()
{
  Serial.begin(115200);

  FastLED.addLeds<NEOPIXEL,MATRIX_PIN>(matrixleds, NUMMATRIX); 
  matrix.begin();
  displayStatus("WiFi");

  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);

  Serial.print("Waiting for WiFi to connect...");
  while ((WiFiMulti.run() != WL_CONNECTED)) {
    Serial.print(".");
  }
  Serial.println(" connected");

  setClock();

  setupAlexa();

  xTaskCreatePinnedToCore(loopBackground, "loopBackground", 4096, NULL, 1, NULL, ARDUINO_RUNNING_CORE);
}

void loopBackground(void *)
{
  for (;;) {
    fauxmo.handle();
  }
}

unsigned long lastUpdateMillis = 0;

void loop()
{
  auto needsUpdate = false;
  auto nowMillis = millis();
  if (isOn && (lastUpdateMillis == 0 || (millis() - lastUpdateMillis) > UPDATE_APPS_MILLIS)) {
    needsUpdate = true;
    lastUpdateMillis = millis();
  }
  
  if (needsUpdate) {
    getApps();
  }
  if (apps.size() == 0) {
    displayStatus("None");
  }
  else {
    for (size_t i = 0; isOn && i < apps.size(); i++) {
      if (needsUpdate || apps[i].buildStatusText.length() == 0) {
        updateApp(apps[i]);
      }
      else {
        delay(1000);
      }
      if (isOn) {
        displayApp(apps[i]);
      }
    }
  }
}
