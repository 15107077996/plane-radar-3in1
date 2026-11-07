#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include "ChinaMap.h"

#define TFT_SDA 8
#define TFT_SCL 9
#define TFT_CS 10
#define TFT_DC 11
#define TFT_RST 12
#define TFT_BL 13

TFT_eSPI tft = TFT_eSPI(240,240);

const char* ssid = "10086";
const char* password = "12345678h";

// 屏幕模式 0时钟 1飞机雷达 2降雨雷达
uint8_t screenMode = 0;
unsigned long lastKey = 0;
#define KEY_PIN 0

// 飞机数据
String planeJson = "";
float planeLat, planeLon, planeAlt, planeSpeed;
String flightNo;

// 雷达扫描角度
int scanAngle = 0;

void drawClock(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.drawCentreString("Clock Mode",120,30,2);
}

void drawPlaneRadar(){
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(120,120,110,TFT_DARKGREY);
  tft.drawCircle(120,120,80,TFT_DARKGREY);
  tft.drawCircle(120,120,50,TFT_DARKGREY);
  int x2 = 120 + 105 * cos(scanAngle * PI / 180.0);
  int y2 = 120 + 105 * sin(scanAngle * PI / 180.0);
  tft.drawLine(120,120,x2,y2,TFT_GREEN);
  scanAngle += 2;
  if(scanAngle >= 360) scanAngle = 0;

  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(1);
  tft.drawString("Flight:"+flightNo,10,200);
  tft.drawString("Alt:"+String(planeAlt)+"m",10,215);
  tft.drawString("Spd:"+String(planeSpeed)+"km/h",10,230);
}

void drawRainRadar(){
  tft.fillScreen(TFT_BLACK);
  drawChinaMap(tft);
  tft.fillCircle(120,120,4,TFT_RED);
  int x2 = 120 + 90 * cos(scanAngle * PI / 180.0);
  int y2 = 120 + 90 * sin(scanAngle * PI / 180.0);
  tft.drawLine(120,120,x2,y2,TFT_CYAN);
  scanAngle +=2;
  if(scanAngle>=360) scanAngle=0;
  tft.setTextColor(TFT_CYAN);
  tft.drawCentreString("Rain Radar",120,10,2);
}

void fetchPlaneData(){
  if(WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://api.adsb.lol/v2/lat/25.83/lon/114.75/dist/50");
  int code = http.GET();
  if(code>0){
    planeJson = http.getString();
    // 简易解析，正式使用建议配JSON库ArduinoJson
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(KEY_PIN,INPUT_PULLUP);
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  WiFi.begin(ssid,password);
  while(WiFi.status() != WL_CONNECTED){
    delay(300);
    tft.drawCentreString("WiFi Connecting",120,100,2);
  }
  tft.drawCentreString("WiFi OK",120,100,2);
  delay(800);
}

void loop() {
  if(digitalRead(KEY_PIN)==LOW && millis()-lastKey>400){
    lastKey = millis();
    screenMode ++;
    if(screenMode>2) screenMode=0;
  }
  switch(screenMode){
    case 0: drawClock(); break;
    case 1: drawPlaneRadar(); fetchPlaneData(); break;
    case 2: drawRainRadar(); break;
  }
  delay(100);
}
