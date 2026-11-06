#include <Adafruit_Arcada.h>
#include <string.h>

Adafruit_Arcada arcada;

constexpr uint8_t CHANNEL_1_PIN = 4;
constexpr uint8_t CHANNEL_2_PIN = 3;
constexpr uint8_t SWCLK_PIN = 30;
constexpr uint8_t SWDIO_PIN = 31;
constexpr uint16_t SAMPLE_COUNT = 320;
constexpr uint16_t PNG_WIDTH = 320;
constexpr uint16_t PNG_IDAT_BUFFER_SIZE = 512;
static_assert(F_CPU == 120000000L,
              "Fast sampling is calibrated for the PyPortal 120 MHz clock");
constexpr uint32_t SAMPLE_PERIODS_NS[] = {
    125,        250,        375,        500,        625,
    750,        875,        1000,       2000,
    5000,       10000,      20000,      50000,      100000,
    200000,     500000,     1000000,    2000000,    5000000,
    10000000,   20000000,   50000000,   100000000,  200000000,
    500000000,  1000000000};
constexpr uint8_t SAMPLE_PERIOD_COUNT =
    sizeof(SAMPLE_PERIODS_NS) / sizeof(SAMPLE_PERIODS_NS[0]);

enum TriggerMode : uint8_t { TRIGGER_AUTO, TRIGGER_RISE, TRIGGER_FALL, TRIGGER_SINGLE };
constexpr TriggerMode TRIGGER_MODES[] = {
    TRIGGER_AUTO, TRIGGER_RISE, TRIGGER_FALL, TRIGGER_SINGLE};
constexpr uint8_t TRIGGER_MODE_COUNT =
    sizeof(TRIGGER_MODES) / sizeof(TRIGGER_MODES[0]);
const char *const TRIGGER_NAMES[] = {"AUTO", "RISE", "FALL", "SINGLE"};

constexpr int16_t PLOT_TOP = 34;
constexpr int16_t PLOT_HEIGHT = 142;
constexpr uint16_t PNG_HEIGHT = PLOT_HEIGHT;
constexpr uint16_t PNG_ROW_BYTES = 1 + PNG_WIDTH / 2;
constexpr uint32_t PNG_RAW_SIZE =
    static_cast<uint32_t>(PNG_ROW_BYTES) * PNG_HEIGHT;
static_assert(PNG_RAW_SIZE <= 65535, "PNG data must fit one DEFLATE block");
constexpr uint32_t SAVE_CAPTURE_DURATION_US = 5000000UL;
constexpr int16_t FOOTER_Y = 185;
constexpr int16_t BUTTON_Y = 200;
constexpr int16_t BUTTON_WIDTH = 70;
constexpr int16_t BUTTON_HEIGHT = 32;
constexpr int16_t BUTTON_X[] = {8, 86, 164, 242};
constexpr int16_t BUTTON_TOUCH_TOP = BUTTON_Y - 10;
constexpr int16_t SAVE_BUTTON_X = 8;
constexpr int16_t SAVE_BUTTON_Y = 1;
constexpr int16_t SAVE_BUTTON_WIDTH = 64;
constexpr int16_t SAVE_BUTTON_HEIGHT = 22;
constexpr uint32_t BUTTON_HIGHLIGHT_MS = 220;
constexpr uint16_t COLOR_BACKGROUND = 0x0841;
constexpr uint16_t COLOR_PANEL = 0x18E3;
constexpr uint16_t COLOR_GRID = 0x2948;
constexpr uint16_t COLOR_CYAN = 0x07FF;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_ORANGE = 0xFD20;

const int16_t CHANNEL_HIGH_OFFSETS[] = {8, 40, 72, 104};
const int16_t CHANNEL_LOW_OFFSETS[] = {20, 52, 84, 116};
const uint16_t CHANNEL_COLORS[] = {
    COLOR_CYAN, COLOR_YELLOW, COLOR_GREEN, COLOR_ORANGE};

uint8_t samples[SAMPLE_COUNT];
uint32_t rawPortSamples[SAMPLE_COUNT];
uint8_t savedSamples[SAMPLE_COUNT];
uint8_t savedSeenHigh[SAMPLE_COUNT];
uint8_t savedSeenLow[SAMPLE_COUNT];
uint8_t triggerIndex = 0;
uint8_t samplePeriodIndex = 4;
bool running = true;
bool previousTouch = false;
uint8_t highlightedButton = 255;
uint32_t buttonHighlightUntil = 0;
uint32_t measuredPeriodNs = 0;
uint32_t capturedPeriodNs = SAMPLE_PERIODS_NS[4];
bool sdReady = false;
bool hasCapture = false;
bool hasFiveSecondCapture = false;
bool saveCaptureActive = false;
bool saveOperationActive = false;
uint32_t saveCaptureStarted = 0;
uint32_t saveNextSample = 0;
uint32_t saveLastStatusSecond = 0;
uint32_t saveSamplePeriodNs = SAMPLE_PERIODS_NS[4];
bool regularCaptureActive = false;
uint16_t regularCaptureIndex = 0;
uint32_t regularCaptureStartUs = 0;
uint32_t regularNextSampleUs = 0;
uint32_t regularCapturePeriodUs = 0;
uint32_t lastCaptureTouchPollMs = 0;
void configureDebugInput(uint8_t pin) {
  uint32_t pinMask = 1ul << pin;
  PORT->Group[0].DIRCLR.reg = pinMask;
  PORT->Group[0].OUTCLR.reg = pinMask;
  PORT->Group[0].PINCFG[pin].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
}

uint8_t readChannels() {
  return (digitalRead(CHANNEL_1_PIN) == HIGH ? 1 : 0) |
         (digitalRead(CHANNEL_2_PIN) == HIGH ? 2 : 0) |
         ((PORT->Group[0].IN.reg & (1ul << SWCLK_PIN)) ? 4 : 0) |
         ((PORT->Group[0].IN.reg & (1ul << SWDIO_PIN)) ? 8 : 0);
}

bool hasRisingEdge(uint8_t previous, uint8_t current) {
  return ((~previous & current) & 0x0F) != 0;
}

bool hasFallingEdge(uint8_t previous, uint8_t current) {
  return ((previous & ~current) & 0x0F) != 0;
}

void drawButton(uint8_t index, const char *text) {
  Adafruit_GFX *display = arcada.display;
  bool highlighted =
      highlightedButton == index &&
      static_cast<int32_t>(buttonHighlightUntil - millis()) > 0;
  uint16_t buttonColor = highlighted ? COLOR_YELLOW : COLOR_GREEN;
  display->fillRoundRect(BUTTON_X[index], BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT,
                         4, highlighted ? COLOR_YELLOW : COLOR_PANEL);
  display->drawRoundRect(BUTTON_X[index], BUTTON_Y, BUTTON_WIDTH,
                         BUTTON_HEIGHT, 4, buttonColor);
  display->setTextSize(1);
  display->setTextColor(COLOR_GREEN);
  int16_t textWidth = strlen(text) * 6;
  display->setCursor(BUTTON_X[index] + (BUTTON_WIDTH - textWidth) / 2,
                     BUTTON_Y + 12);
  display->print(text);
}

void drawSaveButton() {
  Adafruit_GFX *display = arcada.display;
  bool highlighted =
      highlightedButton == 4 &&
      static_cast<int32_t>(buttonHighlightUntil - millis()) > 0;
  uint16_t fillColor = saveOperationActive
                           ? COLOR_RED
                           : (highlighted ? COLOR_YELLOW : COLOR_PANEL);
  uint16_t borderColor = saveOperationActive
                             ? COLOR_RED
                             : (highlighted ? COLOR_YELLOW : COLOR_GREEN);
  display->fillRoundRect(SAVE_BUTTON_X, SAVE_BUTTON_Y, SAVE_BUTTON_WIDTH,
                         SAVE_BUTTON_HEIGHT, 4, fillColor);
  display->drawRoundRect(SAVE_BUTTON_X, SAVE_BUTTON_Y, SAVE_BUTTON_WIDTH,
                         SAVE_BUTTON_HEIGHT, 4, borderColor);
  display->setTextSize(1);
  display->setTextColor(saveOperationActive ? COLOR_WHITE : COLOR_GREEN);
  display->setCursor(SAVE_BUTTON_X + (SAVE_BUTTON_WIDTH - 24) / 2,
                     SAVE_BUTTON_Y + 8);
  display->print("SAVE");
}

void drawControls() {
  drawButton(0, running ? "STOP" : (TRIGGER_MODES[triggerIndex] == TRIGGER_SINGLE
                                        ? "ARM"
                                        : "RUN"));
  drawButton(1, TRIGGER_NAMES[triggerIndex]);
  drawButton(2, "T-");
  drawButton(3, "T+");
  drawSaveButton();
}

void drawTimebaseHeader() {
  Adafruit_GFX *display = arcada.display;
  char text[16];
  uint32_t periodNs = SAMPLE_PERIODS_NS[samplePeriodIndex];
  if (periodNs < 1000) {
    snprintf(text, sizeof(text), "T:%luns",
             static_cast<unsigned long>(periodNs));
  } else if (periodNs < 1000000) {
    snprintf(text, sizeof(text), "T:%luus",
             static_cast<unsigned long>(periodNs / 1000));
  } else if (periodNs < 1000000000) {
    snprintf(text, sizeof(text), "T:%lums",
             static_cast<unsigned long>(periodNs / 1000000));
  } else {
    snprintf(text, sizeof(text), "T:1s");
  }
  display->fillRect(190, 0, display->width() - 190, 24, COLOR_PANEL);
  display->setTextSize(2);
  display->setTextColor(COLOR_GREEN);
  int16_t textWidth = strlen(text) * 12;
  display->setCursor(display->width() - textWidth - 8, 5);
  display->print(text);
}

void drawChannelLabels(uint8_t channelState) {
  const char *const labels[] = {"CH1", "CH2", "CH3", "CH4"};
  Adafruit_GFX *display = arcada.display;
  display->setTextSize(1);
  for (uint8_t channel = 0; channel < 4; ++channel) {
    display->setTextColor(CHANNEL_COLORS[channel]);
    display->setCursor(4, PLOT_TOP + CHANNEL_HIGH_OFFSETS[channel] - 3);
    display->print(labels[channel]);
    display->print((channelState & (1u << channel)) ? " H" : " L");
  }
}

void drawStaticScreen() {
  Adafruit_GFX *display = arcada.display;
  display->fillScreen(COLOR_BACKGROUND);
  display->fillRect(0, 0, display->width(), 24, COLOR_PANEL);
  drawTimebaseHeader();
  drawSaveButton();

  display->drawRect(0, PLOT_TOP, display->width(), PLOT_HEIGHT, COLOR_GRID);
  for (int16_t x = 40; x < display->width(); x += 40) {
    display->drawFastVLine(x, PLOT_TOP, PLOT_HEIGHT, COLOR_GRID);
  }
  for (int16_t y = PLOT_TOP + 20; y < PLOT_TOP + PLOT_HEIGHT; y += 20) {
    display->drawFastHLine(0, y, display->width(), COLOR_GRID);
  }

  drawChannelLabels(readChannels());
  drawControls();
}

void drawFooter(const char *message) {
  (void)message;
}

void showFooterFeedback(const char *message) {
  Serial.println(message);
  drawFooter("");
}

void finishSaveOperation(const char *message) {
  saveOperationActive = false;
  showFooterFeedback(message);
  drawControls();
}

bool writeExact(File &file, const uint8_t *data, size_t length) {
  return file.write(data, length) == length;
}

uint32_t updateCrc32(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
  }
  return crc;
}

void writeBigEndian32(uint8_t *buffer, uint32_t value) {
  buffer[0] = value >> 24;
  buffer[1] = value >> 16;
  buffer[2] = value >> 8;
  buffer[3] = value;
}

bool writePngChunk(File &file, const char type[4], const uint8_t *data,
                   uint32_t length) {
  uint8_t sizeBytes[4];
  writeBigEndian32(sizeBytes, length);
  if (!writeExact(file, sizeBytes, sizeof(sizeBytes)) ||
      !writeExact(file, reinterpret_cast<const uint8_t *>(type), 4) ||
      (length && !writeExact(file, data, length))) {
    return false;
  }

  uint32_t crc = 0xFFFFFFFFUL;
  for (uint8_t i = 0; i < 4; ++i) {
    crc = updateCrc32(crc, type[i]);
  }
  for (uint32_t i = 0; i < length; ++i) {
    crc = updateCrc32(crc, data[i]);
  }
  writeBigEndian32(sizeBytes, crc ^ 0xFFFFFFFFUL);
  return writeExact(file, sizeBytes, sizeof(sizeBytes));
}

struct PngIdatWriter {
  File &file;
  uint8_t buffer[PNG_IDAT_BUFFER_SIZE];
  uint16_t used = 0;
  uint32_t adlerA = 1;
  uint32_t adlerB = 0;
  bool failed = false;

  explicit PngIdatWriter(File &output) : file(output) {}

  bool flushChunk() {
    if (!used) {
      return true;
    }
    bool written = writePngChunk(file, "IDAT", buffer, used);
    used = 0;
    failed = failed || !written;
    return !failed;
  }

  bool writeByte(uint8_t value, bool includeInAdler = false) {
    if (failed) {
      return false;
    }
    buffer[used++] = value;
    if (includeInAdler) {
      adlerA = (adlerA + value) % 65521;
      adlerB = (adlerB + adlerA) % 65521;
    }
    if (used == sizeof(buffer)) {
      return flushChunk();
    }
    return true;
  }

  bool finish() {
    uint32_t adler = (adlerB << 16) | adlerA;
    writeByte(adler >> 24);
    writeByte(adler >> 16);
    writeByte(adler >> 8);
    writeByte(adler);
    return flushChunk();
  }
};

void rgb565ToRgb888(uint16_t color, uint8_t *rgb) {
  uint8_t red = (color >> 11) & 0x1F;
  uint8_t green = (color >> 5) & 0x3F;
  uint8_t blue = color & 0x1F;
  rgb[0] = (red << 3) | (red >> 2);
  rgb[1] = (green << 2) | (green >> 4);
  rgb[2] = (blue << 3) | (blue >> 2);
}

uint8_t pngLabelGlyphRow(char character, uint8_t row) {
  static const uint8_t glyphs[][5] = {
      {7, 4, 4, 4, 7}, {5, 5, 7, 5, 5}, {2, 6, 2, 2, 7},
      {6, 1, 2, 4, 7}, {6, 1, 2, 1, 6}, {5, 5, 7, 1, 1}};
  uint8_t glyphIndex;
  switch (character) {
    case 'C':
      glyphIndex = 0;
      break;
    case 'H':
      glyphIndex = 1;
      break;
    case '1':
      glyphIndex = 2;
      break;
    case '2':
      glyphIndex = 3;
      break;
    case '3':
      glyphIndex = 4;
      break;
    default:
      glyphIndex = 5;
      break;
  }
  return glyphs[glyphIndex][row];
}

uint8_t pngPixelIndex(uint16_t x, uint16_t y) {
  uint8_t colorIndex =
      (x == 0 || x == PNG_WIDTH - 1 || y == 0 || y == PNG_HEIGHT - 1 ||
       x % 40 == 0 || y % 20 == 0)
          ? 1
          : 0;

  for (uint8_t channel = 0; channel < 4; ++channel) {
    uint8_t current = savedSamples[x] & (1u << channel);
    int16_t highY = CHANNEL_HIGH_OFFSETS[channel];
    int16_t lowY = CHANNEL_LOW_OFFSETS[channel];
    int16_t signalY = current ? highY : lowY;
    bool onTrace = y == signalY;
    if (hasFiveSecondCapture &&
        (savedSeenHigh[x] & (1u << channel)) &&
        (savedSeenLow[x] & (1u << channel)) &&
        y >= highY && y <= lowY) {
      onTrace = true;
    } else if (!hasFiveSecondCapture && x > 0) {
      bool previous = savedSamples[x - 1] & (1u << channel);
      if (previous != static_cast<bool>(current) &&
          y >= min(highY, lowY) && y <= max(highY, lowY)) {
        onTrace = true;
      }
    }
    if (onTrace) {
      colorIndex = channel + 2;
    }

    int16_t labelX = x - 4;
    int16_t labelY = y - CHANNEL_HIGH_OFFSETS[channel] + 3;
    if (labelX >= 0 && labelX < 11 && labelY >= 0 && labelY < 5) {
      uint8_t characterIndex = labelX / 4;
      uint8_t glyphColumn = labelX % 4;
      if (characterIndex < 3 && glyphColumn < 3 &&
          (pngLabelGlyphRow(characterIndex == 0 ? 'C'
                            : characterIndex == 1 ? 'H'
                            : channel == 0       ? '1'
                            : channel == 1       ? '2'
                            : channel == 2       ? '3'
                                                 : '4',
                            labelY) &
           (1u << (2 - glyphColumn)))) {
        colorIndex = channel + 2;
      }
    }
  }
  return colorIndex;
}

void recordFiveSecondSample(uint32_t elapsedUs, uint8_t value) {
  uint16_t column = static_cast<uint64_t>(elapsedUs) * SAMPLE_COUNT /
                    SAVE_CAPTURE_DURATION_US;
  if (column >= SAMPLE_COUNT) {
    column = SAMPLE_COUNT - 1;
  }
  savedSamples[column] = value;
  savedSeenHigh[column] |= value;
  savedSeenLow[column] |= static_cast<uint8_t>((~value) & 0x0F);
}

void writeCaptureFiles();

void startSaveCapture() {
  if (!sdReady) {
    sdReady = arcada.filesysBegin(ARCADA_FILESYS_SD) == ARCADA_FILESYS_SD;
    if (!sdReady) {
      showFooterFeedback("SD NOT READY");
      return;
    }
  }
  saveOperationActive = true;
  memset(savedSamples, 0, sizeof(savedSamples));
  memset(savedSeenHigh, 0, sizeof(savedSeenHigh));
  memset(savedSeenLow, 0, sizeof(savedSeenLow));
  hasFiveSecondCapture = true;
  saveCaptureActive = true;
  saveCaptureStarted = micros();
  saveNextSample = 0;
  saveLastStatusSecond = 0;
  saveSamplePeriodNs = SAMPLE_PERIODS_NS[samplePeriodIndex];
  recordFiveSecondSample(0, readChannels());
  drawControls();
  drawFooter("");
}

void updateSaveCapture() {
  if (!saveCaptureActive) {
    return;
  }

  uint32_t elapsed = micros() - saveCaptureStarted;
  uint32_t second = elapsed / 1000000UL;
  if (second > saveLastStatusSecond && second < 5) {
    saveLastStatusSecond = second;
    char status[20];
    snprintf(status, sizeof(status), "CAPTURE %lus",
             static_cast<unsigned long>(5 - second));
    drawFooter(status);
  }

  if (elapsed >= SAVE_CAPTURE_DURATION_US) {
    recordFiveSecondSample(SAVE_CAPTURE_DURATION_US - 1, readChannels());
    saveCaptureActive = false;
    capturedPeriodNs = static_cast<uint64_t>(SAVE_CAPTURE_DURATION_US) * 1000 /
                       (SAMPLE_COUNT - 1);
    writeCaptureFiles();
    return;
  }

  if (static_cast<int32_t>(elapsed - saveNextSample) >= 0) {
    recordFiveSecondSample(elapsed, readChannels());
    saveNextSample += max(1UL, saveSamplePeriodNs / 1000);
  }
}

bool saveCapturePng(const char *path, uint32_t &fileSize) {
  File file = arcada.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    return false;
  }

  static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  uint8_t header[13] = {
      static_cast<uint8_t>(PNG_WIDTH >> 24),
      static_cast<uint8_t>(PNG_WIDTH >> 16),
      static_cast<uint8_t>(PNG_WIDTH >> 8),
      static_cast<uint8_t>(PNG_WIDTH),
      static_cast<uint8_t>(PNG_HEIGHT >> 24),
      static_cast<uint8_t>(PNG_HEIGHT >> 16),
      static_cast<uint8_t>(PNG_HEIGHT >> 8),
      static_cast<uint8_t>(PNG_HEIGHT),
      4,
      3,
      0,
      0,
      0};
  uint8_t palette[18];
  rgb565ToRgb888(COLOR_BACKGROUND, palette);
  rgb565ToRgb888(COLOR_GRID, palette + 3);
  for (uint8_t channel = 0; channel < 4; ++channel) {
    rgb565ToRgb888(CHANNEL_COLORS[channel], palette + (channel + 2) * 3);
  }

  bool ok = writeExact(file, signature, sizeof(signature)) &&
            writePngChunk(file, "IHDR", header, sizeof(header)) &&
            writePngChunk(file, "PLTE", palette, sizeof(palette));
  if (ok) {
    PngIdatWriter idat(file);
    uint8_t zlibHeader[] = {
        0x78, 0x01, 0x01,
        static_cast<uint8_t>(PNG_RAW_SIZE),
        static_cast<uint8_t>(PNG_RAW_SIZE >> 8),
        static_cast<uint8_t>(~PNG_RAW_SIZE),
        static_cast<uint8_t>(~(PNG_RAW_SIZE >> 8))};
    for (uint8_t value : zlibHeader) {
      ok = idat.writeByte(value) && ok;
    }

    for (uint16_t y = 0; ok && y < PNG_HEIGHT; ++y) {
      ok = idat.writeByte(0, true);
      for (uint16_t x = 0; ok && x < PNG_WIDTH; x += 2) {
        uint8_t packed =
            (pngPixelIndex(x, y) << 4) | pngPixelIndex(x + 1, y);
        ok = idat.writeByte(packed, true);
      }
    }
    ok = idat.finish() && ok;
  }

  if (ok) {
    ok = writePngChunk(file, "IEND", NULL, 0);
  }
  bool syncSucceeded = ok && file.sync();
  fileSize = file.size();
  bool writeFailed = file.getWriteError();
  file.close();
  return ok && syncSucceeded && !writeFailed &&
         fileSize > PNG_RAW_SIZE && fileSize < PNG_RAW_SIZE + 1024;
}

bool saveCaptureCsv(const char *path, uint32_t &fileSize) {
  File file = arcada.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    return false;
  }

  static const char header[] = "sample,time_ns,CH1,CH2,CH3,CH4\n";
  uint32_t expectedSize = sizeof(header) - 1;
  bool writeFailed = file.write(header, sizeof(header) - 1) != sizeof(header) - 1;
  for (uint16_t sample = 0; !writeFailed && sample < SAMPLE_COUNT; ++sample) {
    char row[64];
    int rowLength = snprintf(
        row, sizeof(row), "%u,%llu,%u,%u,%u,%u\n",
        static_cast<unsigned int>(sample),
        static_cast<unsigned long long>(sample) * capturedPeriodNs,
        (savedSamples[sample] & (1u << 0)) ? 1u : 0u,
        (savedSamples[sample] & (1u << 1)) ? 1u : 0u,
        (savedSamples[sample] & (1u << 2)) ? 1u : 0u,
        (savedSamples[sample] & (1u << 3)) ? 1u : 0u);
    if (rowLength <= 0 || rowLength >= sizeof(row) ||
        file.write(row, rowLength) != static_cast<size_t>(rowLength)) {
      writeFailed = true;
      break;
    }
    expectedSize += static_cast<uint32_t>(rowLength);
  }

  bool syncSucceeded = !writeFailed && file.sync();
  writeFailed = writeFailed || file.getWriteError();
  fileSize = file.size();
  file.close();
  return !writeFailed && syncSucceeded && fileSize == expectedSize;
}

void drawWaveform() {
  Adafruit_GFX *display = arcada.display;
  display->fillRect(1, PLOT_TOP + 1, display->width() - 2, PLOT_HEIGHT - 2,
                    COLOR_BACKGROUND);
  for (int16_t x = 40; x < display->width(); x += 40) {
    display->drawFastVLine(x, PLOT_TOP + 1, PLOT_HEIGHT - 2, COLOR_GRID);
  }
  for (int16_t y = PLOT_TOP + 20; y < PLOT_TOP + PLOT_HEIGHT; y += 20) {
    display->drawFastHLine(1, y, display->width() - 2, COLOR_GRID);
  }

  drawChannelLabels(samples[SAMPLE_COUNT - 1]);

  uint8_t previous = samples[0];
  int16_t previousX = 42;
  int16_t previousY[4];
  for (uint8_t channel = 0; channel < 4; ++channel) {
    previousY[channel] =
        PLOT_TOP + ((previous & (1u << channel))
                        ? CHANNEL_HIGH_OFFSETS[channel]
                        : CHANNEL_LOW_OFFSETS[channel]);
    display->drawPixel(previousX, previousY[channel],
                       CHANNEL_COLORS[channel]);
  }

  for (uint16_t i = 1; i < SAMPLE_COUNT; ++i) {
    uint8_t current = samples[i];
    int16_t x = 42 + static_cast<uint32_t>(i) *
                         (display->width() - 43) / (SAMPLE_COUNT - 1);
    for (uint8_t channel = 0; channel < 4; ++channel) {
      int16_t currentY =
          PLOT_TOP + ((current & (1u << channel))
                          ? CHANNEL_HIGH_OFFSETS[channel]
                          : CHANNEL_LOW_OFFSETS[channel]);
      if (currentY == previousY[channel]) {
        if (x > previousX) {
          display->drawFastHLine(previousX + 1, currentY, x - previousX,
                                 CHANNEL_COLORS[channel]);
        }
      } else {
        display->drawFastVLine(x, min(currentY, previousY[channel]),
                               abs(currentY - previousY[channel]) + 1,
                               CHANNEL_COLORS[channel]);
      }
      display->drawPixel(x, currentY, CHANNEL_COLORS[channel]);
      previousY[channel] = currentY;
    }

    previousX = x;
  }
}

bool waitForTrigger(TriggerMode mode, uint8_t &triggerValue) {
  if (mode == TRIGGER_AUTO) {
    triggerValue = readChannels();
    return true;
  }

  drawFooter(mode == TRIGGER_FALL ? "WAIT FALL" : "WAIT RISE");
  uint8_t previous = readChannels();
  uint32_t started = micros();
  while (static_cast<uint32_t>(micros() - started) < 1000000UL) {
    updateSaveCapture();
    uint8_t current = readChannels();
    if ((mode == TRIGGER_FALL && hasFallingEdge(previous, current)) ||
        (mode != TRIGGER_FALL && hasRisingEdge(previous, current))) {
      triggerValue = current;
      return true;
    }
    previous = current;
  }
  drawFooter("NO EDGE");
  return false;
}

#define CAPTURE_PORT_LOOP(NOPS)                                             \
  asm volatile(                                                             \
      "1:\n\t"                                                             \
      "ldr r3, [%[port]]\n\t"                                               \
      "str r3, [%[destination]], #4\n\t"                                     \
      ".rept " #NOPS "\n\t"                                                \
      "nop\n\t"                                                             \
      ".endr\n\t"                                                           \
      "subs %[remaining], %[remaining], #1\n\t"                             \
      "bne 1b\n\t"                                                          \
      : [destination] "+l"(destination), [remaining] "+l"(remaining)       \
      : [port] "l"(portInput)                                               \
      : "r3", "memory", "cc")

uint32_t capturePortBurst(uint32_t periodNs) {
  volatile uint32_t *portInput = &PORT->Group[0].IN.reg;
  uint32_t *destination = rawPortSamples;
  uint32_t remaining = SAMPLE_COUNT;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t startedCycles = DWT->CYCCNT;
  switch (periodNs) {
    case 125:
      CAPTURE_PORT_LOOP(7);
      break;
    case 250:
      CAPTURE_PORT_LOOP(22);
      break;
    case 375:
      CAPTURE_PORT_LOOP(37);
      break;
    case 500:
      CAPTURE_PORT_LOOP(52);
      break;
    case 625:
      CAPTURE_PORT_LOOP(67);
      break;
    case 750:
      CAPTURE_PORT_LOOP(82);
      break;
    case 875:
      CAPTURE_PORT_LOOP(97);
      break;
    default:
      CAPTURE_PORT_LOOP(112);
      break;
  }
  uint32_t elapsedCycles = DWT->CYCCNT - startedCycles;
  __set_PRIMASK(primask);
  return elapsedCycles;
}

#undef CAPTURE_PORT_LOOP

bool captureSamples() {
  uint32_t periodNs = SAMPLE_PERIODS_NS[samplePeriodIndex];
  if (periodNs <= 1000) {
    uint8_t firstValue;
    if (!waitForTrigger(TRIGGER_MODES[triggerIndex], firstValue)) {
      return false;
    }
    uint32_t elapsedCycles = capturePortBurst(periodNs);

    for (uint16_t i = 0; i < SAMPLE_COUNT; ++i) {
      uint32_t pins = rawPortSamples[i];
      samples[i] = ((pins >> 5) & 1u) | (((pins >> 4) & 1u) << 1) |
                   (((pins >> 30) & 1u) << 2) |
                   (((pins >> 31) & 1u) << 3);
    }
    measuredPeriodNs =
        static_cast<uint64_t>(elapsedCycles) * 1000000000ULL /
        (static_cast<uint64_t>(F_CPU) * SAMPLE_COUNT);
    capturedPeriodNs = measuredPeriodNs;
    hasCapture = true;
    return true;
  }

  uint8_t firstValue;
  if (!regularCaptureActive) {
    if (!waitForTrigger(TRIGGER_MODES[triggerIndex], firstValue)) {
      return false;
    }
    samples[0] = firstValue;
    for (uint16_t i = 1; i < SAMPLE_COUNT; ++i) {
      samples[i] = firstValue;
    }
    regularCaptureIndex = 1;
    regularCaptureStartUs = micros();
    regularCapturePeriodUs = periodNs / 1000;
    regularNextSampleUs = regularCaptureStartUs + regularCapturePeriodUs;
    regularCaptureActive = true;
    return false;
  }

  if (periodNs < 1000000) {
    uint32_t periodUs = periodNs / 1000;
    uint32_t target =
        regularCaptureStartUs +
        static_cast<uint32_t>(regularCaptureIndex) * periodUs;
    while (static_cast<int32_t>(micros() - target) < 0) {
      updateSaveCapture();
    }
    samples[regularCaptureIndex++] = readChannels();
    if (regularCaptureIndex < SAMPLE_COUNT) {
      return false;
    }
    uint32_t elapsed = micros() - regularCaptureStartUs;
    measuredPeriodNs = static_cast<uint64_t>(elapsed) * 1000 /
                       (SAMPLE_COUNT - 1);
    capturedPeriodNs = periodNs;
    regularCaptureActive = false;
    hasCapture = true;
    return true;
  }

  uint32_t now = micros();
  if (static_cast<int32_t>(now - regularNextSampleUs) < 0) {
    return false;
  }
  samples[regularCaptureIndex++] = readChannels();
  regularNextSampleUs += regularCapturePeriodUs;
  if (regularCapturePeriodUs >= 100000UL) {
    drawWaveform();
  }

  if (regularCaptureIndex < SAMPLE_COUNT) {
    return false;
  }

  uint32_t elapsed = now - regularCaptureStartUs;
  measuredPeriodNs = static_cast<uint64_t>(elapsed) * 1000 /
                     (SAMPLE_COUNT - 1);
  capturedPeriodNs = periodNs;
  regularCaptureActive = false;
  hasCapture = true;
  return true;
}

void writeCaptureFiles() {
  char pngPath[15];
  char csvPath[15];
  char pngFilename[13];
  char csvFilename[13];
  for (uint16_t index = 0; index < 1000; ++index) {
    snprintf(pngFilename, sizeof(pngFilename), "LOG%03u.PNG",
             static_cast<unsigned int>(index));
    snprintf(csvFilename, sizeof(csvFilename), "LOG%03u.CSV",
             static_cast<unsigned int>(index));
    snprintf(pngPath, sizeof(pngPath), "/%s", pngFilename);
    snprintf(csvPath, sizeof(csvPath), "/%s", csvFilename);
    if (!arcada.exists(pngPath) && !arcada.exists(csvPath)) {
      uint32_t pngSize = 0;
      if (!saveCapturePng(pngPath, pngSize)) {
        finishSaveOperation("PNG SAVE ERROR");
        return;
      }

      File verifyPng = arcada.open(pngPath, O_READ);
      bool pngVerified = verifyPng && verifyPng.size() == pngSize;
      if (verifyPng) {
        verifyPng.close();
      }
      if (!pngVerified) {
        finishSaveOperation("PNG VERIFY ERROR");
        return;
      }

      uint32_t csvSize = 0;
      if (!saveCaptureCsv(csvPath, csvSize)) {
        finishSaveOperation("PNG OK CSV ERROR");
        return;
      }
      File verifyCsv = arcada.open(csvPath, O_READ);
      bool csvVerified = verifyCsv && verifyCsv.size() == csvSize;
      if (verifyCsv) {
        verifyCsv.close();
      }
      if (!csvVerified) {
        finishSaveOperation("PNG OK CSV VERIFY");
        return;
      }

      char message[24];
      snprintf(message, sizeof(message), "SAVED %.11s", pngFilename);
      finishSaveOperation(message);
      return;
    }
  }

  finishSaveOperation("SD FILE LIMIT");
}

void saveCapture() {
  if (saveCaptureActive || saveOperationActive) {
    return;
  }

  if (SAMPLE_PERIODS_NS[samplePeriodIndex] <= 1000) {
    if (!sdReady) {
      sdReady = arcada.filesysBegin(ARCADA_FILESYS_SD) == ARCADA_FILESYS_SD;
      if (!sdReady) {
        showFooterFeedback("SD NOT READY");
        return;
      }
    }

    saveOperationActive = true;
    drawControls();
    if (!captureSamples()) {
      finishSaveOperation("NO TRIGGER");
      return;
    }
    memcpy(savedSamples, samples, sizeof(samples));
    memset(savedSeenHigh, 0, sizeof(savedSeenHigh));
    memset(savedSeenLow, 0, sizeof(savedSeenLow));
    hasFiveSecondCapture = false;
    writeCaptureFiles();
    return;
  }

  startSaveCapture();
}

void updateTouchControls() {
  TSPoint point = arcada.getTouchscreenPoint();
  bool touched = point.z > 100;
  if (touched && !previousTouch) {
    int8_t button = -1;
    if (point.y <= SAVE_BUTTON_Y + SAVE_BUTTON_HEIGHT + 4 &&
        point.x <= SAVE_BUTTON_X + SAVE_BUTTON_WIDTH + 4) {
      button = 4;
    } else if (point.y >= BUTTON_TOUCH_TOP) {
      for (uint8_t i = 0; i < 4; ++i) {
        int16_t touchLeft = i == 0 ? 0 : BUTTON_X[i] - 4;
        int16_t touchRight = i == 3
                                 ? arcada.display->width()
                                 : BUTTON_X[i] + BUTTON_WIDTH + 4;
        if (point.x >= touchLeft && point.x < touchRight) {
          button = i;
          break;
        }
      }
    }

    if (button >= 0) {
      uint8_t i = static_cast<uint8_t>(button);
      highlightedButton = i;
      buttonHighlightUntil = millis() + BUTTON_HIGHLIGHT_MS;
      if (i == 0) {
        running = !running;
        if (!running) {
          regularCaptureActive = false;
        }
        drawFooter(running ? "RUNNING" : "PAUSED");
      } else if (i == 1) {
        triggerIndex = (triggerIndex + 1) % TRIGGER_MODE_COUNT;
        regularCaptureActive = false;
        if (TRIGGER_MODES[triggerIndex] == TRIGGER_AUTO) {
          running = true;
        }
        drawFooter(TRIGGER_NAMES[triggerIndex]);
      } else if (i == 2) {
        if (samplePeriodIndex > 0) {
          --samplePeriodIndex;
          regularCaptureActive = false;
        }
        drawTimebaseHeader();
      } else if (i == 3) {
        if (samplePeriodIndex + 1 < SAMPLE_PERIOD_COUNT) {
          ++samplePeriodIndex;
          regularCaptureActive = false;
        }
        drawTimebaseHeader();
      } else if (i == 4) {
        saveCapture();
      }
      drawControls();
    }
  }
  previousTouch = touched;
}

void setup() {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  pinMode(CHANNEL_1_PIN, INPUT_PULLDOWN);
  pinMode(CHANNEL_2_PIN, INPUT_PULLDOWN);
  configureDebugInput(SWCLK_PIN);
  configureDebugInput(SWDIO_PIN);

  if (!arcada.arcadaBegin()) {
    while (true) {
      delay(100);
    }
  }
  arcada.displayBegin();
  arcada.setBacklight(255);
  sdReady = arcada.filesysBegin(ARCADA_FILESYS_SD) == ARCADA_FILESYS_SD;
  drawStaticScreen();
  drawFooter(sdReady ? TRIGGER_NAMES[triggerIndex] : "SD NOT READY");
}

void loop() {
  updateSaveCapture();
  if (highlightedButton < 5 &&
      static_cast<int32_t>(millis() - buttonHighlightUntil) >= 0) {
    highlightedButton = 255;
    drawControls();
  }

  if (!running) {
    drawChannelLabels(readChannels());
    updateTouchControls();
    delay(20);
    return;
  }

  drawFooter(TRIGGER_NAMES[triggerIndex]);

  bool captured = captureSamples();
  if (captured) {
    drawWaveform();
    drawFooter(TRIGGER_NAMES[triggerIndex]);
  }

  if (!regularCaptureActive) {
    drawChannelLabels(readChannels());
  }
  uint32_t nowMs = millis();
  if (!regularCaptureActive || nowMs - lastCaptureTouchPollMs >= 20) {
    lastCaptureTouchPollMs = nowMs;
    updateTouchControls();
  }
  if (!captured && regularCaptureActive &&
      SAMPLE_PERIODS_NS[samplePeriodIndex] >= 1000000) {
    delay(1);
  }
  if (captured && TRIGGER_MODES[triggerIndex] == TRIGGER_SINGLE) {
    running = false;
    drawControls();
  }
}
