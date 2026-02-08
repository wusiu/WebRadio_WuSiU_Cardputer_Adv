#include "M5Cardputer.h"
#include "CardWifiSetup.h"
#include <Audio.h>
#include <SD.h>

#define MAX_STATIONS 20
#define MAX_NAME_LENGTH 30
#define MAX_URL_LENGTH 100
#define I2S_BCK 41
#define I2S_WS 43
#define I2S_DOUT 42

#define VOLUME_STEP 20
#define FOOTER_HEIGHT 16
uint8_t brightnessLevels[5] = {0, 30, 100, 200, 255};
uint8_t currentBrightnessIndex = 4;

#define FFT_SIZE 256
#define WAVE_SIZE 320
static uint16_t prev_y[(FFT_SIZE / 2)+1];
static uint16_t peak_y[(FFT_SIZE / 2)+1];
static int header_height = 50;
static bool fft_enabled = false;
static bool fftSimON = true;

Audio audio;

bool stationMenuActive = false;
bool fftWasEnabled = false;
int menuIndex = 0;
const int MENU_TOP = 15;
const int MENU_VISIBLE = 6;
int MENU_LINE_H = 0;

char currentStreamTitle[128] = "";
bool streamTitleChanged = false;

unsigned long lastUpdate = 0;

class fft_t {
public:
  fft_t() {
    for (int i = 0; i < FFT_SIZE; i++) {
      _data[i] = 0;
    }
  }

  void exec(const int16_t* in) {
    if (fftSimON) {
      for (int i = 0; i < FFT_SIZE; i++) {
        _data[i] = abs(in[i]);
      }
    }
  }

  uint32_t get(size_t index) {
    if (index < FFT_SIZE) {
      return _data[index];
    }
    return 0;
  }

  private:
    uint32_t _data[FFT_SIZE];
};

static fft_t fft;
static int16_t raw_data[WAVE_SIZE * 2];

static uint32_t bgcolor(int y) {
  auto h = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  auto dh = h - header_height;

  int v = ((h - y) << 5) / dh;
  if (dh > header_height) {
    int v2 = ((h - y - 1) << 5) / dh;
    if ((v >> 2) != (v2 >> 2)) {
      return 0x666666u;
    }
  }
  return M5Cardputer.Display.color888(v + 2, v, v + 6);
}

struct RadioStation {
  char name[MAX_NAME_LENGTH];
  char url[MAX_URL_LENGTH];
};

const PROGMEM RadioStation defaultStations[] = {
  {"RMF FM", "http://rmfstream1.interia.pl:80/rmf_fm"},
};

RadioStation stations[MAX_STATIONS];
size_t numStations = 0;
size_t curStation = 0;
uint16_t curVolume = 115;
uint8_t lastVolumeDrawn = 255;

unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

void setupFFT() {
  if (!fft_enabled) return;
  
  for (int x = 0; x < (FFT_SIZE / 2) + 1; ++x) {
    prev_y[x] = INT16_MAX;
    peak_y[x] = INT16_MAX;
  }

  int display_height = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  for (int y = header_height; y < display_height; ++y) {
    M5Cardputer.Display.drawFastHLine(0, y, M5Cardputer.Display.width(), bgcolor(y));
  }
}

void updateFFT() {
  if (!fft_enabled) return;

  static unsigned long lastFFTUpdate = 0;
  if (millis() - lastFFTUpdate < 50) return;
  lastFFTUpdate = millis();

  for (int i = 0; i < WAVE_SIZE * 2; i++) {
    raw_data[i] = random(-32000, 32000);
  }

  fft.exec(raw_data);

  size_t bw = M5Cardputer.Display.width() / 30;
  if (bw < 3) bw = 3;
  int32_t dsp_height = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  int32_t fft_height = dsp_height - header_height - 1;
  size_t xe = M5Cardputer.Display.width() / bw;
  if (xe > (FFT_SIZE / 2)) xe = (FFT_SIZE / 2);

  uint32_t bar_color[2] = {0x000033u, 0x99AAFFu};

  M5Cardputer.Display.startWrite();
  
  int32_t bottom = M5Cardputer.Display.height() - FOOTER_HEIGHT - 1;

  for (size_t bx = 0; bx <= xe; ++bx) {
    size_t x = bx * bw;
    int32_t f = fft.get(bx) * 3;
    int32_t y = (f * fft_height) >> 17;
    if (y > fft_height) y = fft_height;
    y = dsp_height - y;
    if (y > bottom) y = bottom;
    int32_t py = prev_y[bx];

    if (py > bottom) py = bottom;

    if (y != py) {
      M5Cardputer.Display.fillRect(x, y, bw - 1, py - y, bar_color[(y < py)]);
      prev_y[bx] = y;
    }

    py = peak_y[bx] + ((peak_y[bx] - y) > 5 ? 2 : 1);
    if (py < y) {
      M5Cardputer.Display.writeFastHLine(x, py - 1, bw - 1, bgcolor(py - 1));
    } else {
      py = y - 1;
    }
    if (peak_y[bx] != py) {
      peak_y[bx] = py;
      M5Cardputer.Display.writeFastHLine(x, py, bw - 1, TFT_WHITE);
    }
  }
  
  M5Cardputer.Display.endWrite();
}

void toggleFFT() {
  fft_enabled = !fft_enabled;

  M5Cardputer.Display.fillRect(
    0,
    header_height,
    M5Cardputer.Display.width(),
    M5Cardputer.Display.height() - header_height - FOOTER_HEIGHT,
    TFT_BLACK
  );

  if (fft_enabled) {
    setupFFT();
  }

  drawFooter();
}

void updateBatteryDisplay(unsigned long updateInterval) {
  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate < updateInterval) return;
  lastUpdate = millis();

  int batteryLevel = M5.Power.getBatteryLevel();
  const int batteryY = 0;

  uint16_t batteryColor = batteryLevel < 30 ? TFT_RED : TFT_GREEN;

  char percentStr[6];
  snprintf(percentStr, sizeof(percentStr), "%d%%", batteryLevel);

  const int battW = 20;
  const int battH = 10;
  const int tipW  = 3;
  const int gap   = 4;

  const int percentMaxW = M5Cardputer.Display.textWidth("100%");

  int percentX = 240 - percentMaxW - 2;
  int battX    = percentX - gap - battW;

  M5Cardputer.Display.fillRect(
    battX - 4,
    batteryY,
    percentMaxW + battW + gap + 10,
    14,
    TFT_BLACK
  );

  M5Cardputer.Display.fillRect(battX, batteryY + 2, battW, battH, TFT_DARKGREY);
  M5Cardputer.Display.fillRect(battX + battW, batteryY + 4, tipW, 6, TFT_DARKGREY);
  M5Cardputer.Display.fillRect(
    battX + 2,
    batteryY + 4,
    (batteryLevel * (battW - 4)) / 100,
    6,
    batteryColor
);

  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString(percentStr, percentX, batteryY);

}

void loadDefaultStations() {
  numStations = std::min(sizeof(defaultStations)/sizeof(defaultStations[0]), static_cast<size_t>(MAX_STATIONS));
  memcpy(stations, defaultStations, sizeof(RadioStation) * numStations);
}

void mergeRadioStations() {
  if (!SD.begin()) {
    M5Cardputer.Display.drawString("/station_list.txt ", 20, 30);
    M5Cardputer.Display.drawString("Not found on SD card.", 20, 50);
    delay(4000);
    loadDefaultStations();
    M5Cardputer.Display.fillScreen(BLACK);
    return;
  }

  File file = SD.open("/station_list.txt");
  if (!file) {
    loadDefaultStations();
    return;
  }

  numStations = 0;
  
  String line;
  while (file.available() && numStations < MAX_STATIONS) {
    line = file.readStringUntil('\n');
    int commaIndex = line.indexOf(',');
    
    if (commaIndex > 0) {
      String name = line.substring(0, commaIndex);
      String url = line.substring(commaIndex + 1);
      
      name.trim();
      url.trim();
      
      if (name.length() > 0 && url.length() > 0) {
        strncpy(stations[numStations].name, name.c_str(), MAX_NAME_LENGTH - 1);
        strncpy(stations[numStations].url, url.c_str(), MAX_URL_LENGTH - 1);
        stations[numStations].name[MAX_NAME_LENGTH - 1] = '\0';
        stations[numStations].url[MAX_URL_LENGTH - 1] = '\0';
        numStations++;
      }
    }
  }

  file.close();
  if (numStations == 0) {
    loadDefaultStations();
  }

}

void showStation() {
  M5Cardputer.Display.fillRect(0, 15, 240, 35, TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString(stations[curStation].name, 0, 15);

  M5Cardputer.Display.fillRect(0, 33, 240, 15, TFT_BLACK);
  
  if (currentStreamTitle[0] != '\0') {
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString(currentStreamTitle, 0, 33);
  }

  showVolume();
}

void audio_id3data(const char *info){M5Cardputer.Display.drawString(info, 0, 33);}

void Playfile() {
  audio.stopSong();

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  const char* url = stations[curStation].url;

  if (strstr(url, "http")) {
    audio.connecttohost(url);
  }
  else if (strstr(url, "/mp3")) {
    M5Cardputer.Display.drawString("Play MP3 z SD", 0, 15);
    audio.connecttoFS(SD, url);
  }
  else {
    audio.connecttospeech(
      "Trabalhe em quanto os outros dormem, e você ficará com sono durante o dia.",
      "pt"
    );
  }

  showStation();
}

void volumeUp() {
  if (curVolume >= 255) return;

  if (curVolume + VOLUME_STEP > 255)
    curVolume = 255;
  else
    curVolume += VOLUME_STEP;

  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

void volumeDown() {
  if (curVolume == 0) return;

  if (curVolume <= VOLUME_STEP)
    curVolume = 0;
  else
    curVolume -= VOLUME_STEP;

  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

bool isMuted = false;
uint16_t prevVolume = 0;

void volumeMute() {
  if (!isMuted) {
    prevVolume = curVolume;
    curVolume = 0;
    isMuted = true;
  } else {
    curVolume = prevVolume;
    isMuted = false;
  }
  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

void showVolume() {
  if (curVolume == lastVolumeDrawn) return;
  lastVolumeDrawn = curVolume;

  const int rightReserved = 80;
  const int barHeight = 4;
  const int barY = 6;

  int maxBarWidth = M5Cardputer.Display.width() - rightReserved;
  if (maxBarWidth < 10) return;

  M5Cardputer.Display.fillRect(0, barY, maxBarWidth, barHeight + 2, TFT_BLACK);

  int barWidth = map(curVolume, 0, 255, 0, maxBarWidth);

  if (barWidth > 0) {
    M5Cardputer.Display.fillRect(0, barY, barWidth, barHeight, 0xAAFFAA);
  }
}

void stationUp() {
  if (numStations > 0) {
    currentStreamTitle[0] = '\0';
    streamTitleChanged = true;

    curStation = (curStation + 1) % numStations; 
    audio.stopSong();
    Playfile();
    showStation();
  }
  showVolume();
}

void stationDown() {

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  if (numStations > 0) {
    curStation = (curStation - 1 + numStations) % numStations; 
    audio.stopSong();
    Playfile();
    showStation();
  }
  showVolume();
}

void redrawUI() {
  fftSimON = true;

  showStation();
  lastVolumeDrawn = 255;
  showVolume();
  updateBatteryDisplay(0);
  drawFooter();

  if (fft_enabled) {
    setupFFT();
  }
}

void setup() {
  auto cfg = M5.config();
  auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate = 44100;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5Cardputer.Speaker.config(spk_cfg);
  
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);

  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::FreeMonoOblique9pt7b);

  MENU_LINE_H = M5Cardputer.Display.fontHeight() + 2;
 
  connectToWiFi();
  
  audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT);
  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  audio.setBalance(0);
  
  M5Cardputer.Display.fillScreen(BLACK);  
  
  audio.stopSong();
  mergeRadioStations();
  Playfile();
  toggleFFT();
  redrawUI();
}

void loop() {
  audio.loop();
  M5Cardputer.update();

  if (stationMenuActive) {
    delay(1);
    if (M5Cardputer.Keyboard.isChange() &&
      millis() - lastButtonPress > DEBOUNCE_DELAY) {

      if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        menuIndex = (menuIndex + 1) % numStations;
        drawStationMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        menuIndex = (menuIndex - 1 + numStations) % numStations;
        drawStationMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        curStation = menuIndex;

        currentStreamTitle[0] = '\0';
        streamTitleChanged = true;

        audio.stopSong();
        Playfile();

        stationMenuActive = false;
        fft_enabled = fftWasEnabled;
        fftSimON = true;

        M5Cardputer.Display.fillRect(0, 0, 240, 14, TFT_BLACK);

        if (!fft_enabled) {
          M5Cardputer.Display.fillRect(
            0,
            header_height,
            240,
            M5Cardputer.Display.height() - header_height - FOOTER_HEIGHT,
            TFT_BLACK
          );
        }

        redrawUI();

      }

      else if (M5Cardputer.Keyboard.isKeyPressed('l')) {
        stationMenuActive = false;
        fft_enabled = fftWasEnabled;
        fftSimON = true;

        M5Cardputer.Display.fillRect(0, 0, 240, 14, TFT_BLACK);

        if (!fft_enabled) {
          M5Cardputer.Display.fillRect(
            0,
            header_height,
            240,
            M5Cardputer.Display.height() - header_height - FOOTER_HEIGHT,
            TFT_BLACK
          );
        }

        redrawUI();


      }

      lastButtonPress = millis();

    }

    return;
  }

  updateBatteryDisplay(5000);

  if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {

    if (M5Cardputer.Keyboard.isKeyPressed(';')) volumeUp();
    else if (M5Cardputer.Keyboard.isKeyPressed('.')) volumeDown();
    else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
    else if (M5Cardputer.Keyboard.isKeyPressed('/')) stationUp();
    else if (M5Cardputer.Keyboard.isKeyPressed(',')) stationDown();
    else if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      audio.stopSong();
      audio.connecttohost(stations[curStation].url);
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('f')) {
      toggleFFT();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('b')) {
      toggleBrightness();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('l')) {
      openStationMenu();
    }

    
    lastButtonPress = millis();

   }
  
  if (fft_enabled) {
    updateFFT();
  }
  
  drawStreamTitle();

  delay(1);
}

void audio_showstation(const char *showstation) {
    if (showstation && *showstation) {
        char limitedInfo[241];
        strncpy(limitedInfo, showstation, 240);
        limitedInfo[240] = '\0';
        M5Cardputer.Display.fillRect(0, 15, 240, 15, TFT_BLACK);
        M5Cardputer.Display.drawString(limitedInfo, 0, 15);
       fftSimON = true;
    }
}

void audio_showstreamtitle(const char *info) {
  if (!info || !*info) return;

  strncpy(currentStreamTitle, info, sizeof(currentStreamTitle) - 1);
  currentStreamTitle[sizeof(currentStreamTitle) - 1] = '\0';
  streamTitleChanged = true;
}

void drawStreamTitle() {

  if (stationMenuActive) return;

  static int lastX = -10000;
  static int xOffset = 0;
  static bool scrolling = false;
  static bool scrollDone = false;
  static unsigned long lastUpdate = 0;

  const int screenW = M5Cardputer.Display.width();
  const int startScrollX = 80;
  const int yText = 33;
  const int hText = 15;
  const int scrollSpeed = 20;

  M5Cardputer.Display.fillRect(0, 50, screenW, 1, TFT_RED);

  if (!currentStreamTitle[0]) {
    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
    return;
  }

  int textWidth = M5Cardputer.Display.textWidth(currentStreamTitle);

  if (streamTitleChanged) {
    xOffset = startScrollX;
    scrolling = false;
    scrollDone = false;
    streamTitleChanged = false;
    lastX = -10000;

    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
  }

  if (textWidth <= screenW) {
    if (lastX != 0) {
      lastX = 0;
      M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
      M5Cardputer.Display.drawString(currentStreamTitle, 0, yText);
    }
    return;
  }

  if (scrollDone) {
    if (lastX != 0) {
      lastX = 0;
      M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
      M5Cardputer.Display.drawString(currentStreamTitle, 0, yText);
    }
    return;
  }

  if (!scrolling) {
    scrolling = true;
    xOffset = startScrollX;
    lastUpdate = millis();
  }

  if (millis() - lastUpdate < scrollSpeed) return;
  lastUpdate = millis();

  xOffset--;

  if (xOffset < -textWidth) {
    scrollDone = true;
    scrolling = false;
    return;
  }

  if (xOffset != lastX) {
    lastX = xOffset;
    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
    M5Cardputer.Display.drawString(currentStreamTitle, xOffset, yText);
  }

}

void drawFooter() {
  int y = M5Cardputer.Display.height() - FOOTER_HEIGHT;

  M5Cardputer.Display.fillRect(0, y, 240, FOOTER_HEIGHT, TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawCentreString("@WuSiU", 120, y + 1);
}

void toggleBrightness() {
  currentBrightnessIndex++;
  if (currentBrightnessIndex >= 5) {
    currentBrightnessIndex = 0;
  }

  uint8_t brightness = brightnessLevels[currentBrightnessIndex];
  M5Cardputer.Display.setBrightness(brightness);

}

void drawStationMenu() {
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  M5Cardputer.Display.drawCentreString("Select station:", 120, 0);
  M5Cardputer.Display.fillRect(0, 13, 240, 1, TFT_RED);

  int start = menuIndex - MENU_VISIBLE / 2;
  if (start < 0) start = 0;
  if (start > (int)numStations - MENU_VISIBLE)
    start = max(0, (int)numStations - MENU_VISIBLE);

  const int listTop = MENU_TOP;
  const int listHeight = MENU_VISIBLE * MENU_LINE_H + 30;
  M5Cardputer.Display.fillRect(0, listTop, 240, listHeight, TFT_BLACK);

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = start + i;
    if (idx >= numStations) break;

    int y = MENU_TOP + i * MENU_LINE_H + 8;

    if (idx == menuIndex) {
      M5Cardputer.Display.fillRect(0, y, 240, MENU_LINE_H, TFT_WHITE);
      M5Cardputer.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    } else {
      M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    int textY = y + (MENU_LINE_H - M5Cardputer.Display.fontHeight()) / 2;
    M5Cardputer.Display.drawString(stations[idx].name, 4, textY);
  }
}

void openStationMenu() {
  stationMenuActive = true;
  menuIndex = curStation;

  fftWasEnabled = fft_enabled;
  fft_enabled = false;
  fftSimON = false;

  M5Cardputer.Display.fillRect(0, 0, 240, 135, TFT_BLACK);

  drawStationMenu();
}

