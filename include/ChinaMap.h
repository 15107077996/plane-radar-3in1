#ifndef CHINAMAP_H
#define CHINAMAP_H
#include <TFT_eSPI.h>

void drawChinaMap(TFT_eSPI &tft){
  tft.drawCircle(120,120,100,TFT_DARKBLUE);
  tft.drawCircle(120,120,98,TFT_DARKBLUE);
  tft.drawLine(80,90,160,90,TFT_DARKBLUE);
  tft.drawLine(70,120,170,120,TFT_DARKBLUE);
  tft.drawLine(85,150,155,150,TFT_DARKBLUE);
}
#endif
