#include <GxEPD2_3C.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoLowPower.h>

#define SdFile File
#define seekSet seek
#define SD_CS 4
#define WAKE_PIN 15
#define EINK_CS 19
#define EINK_DC 18
#define EINK_RST 17
#define EINK_BUSY 161
#define DELAY_SEC 5*60
#define SLIDESHOW_ROOT "SLIDES"

GxEPD2_3C<GxEPD2_290c, GxEPD2_290c::HEIGHT> display(GxEPD2_290c(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY));

// function declaration with default parameter
void drawBitmapFromSD(const char *filename, int16_t x, int16_t y, bool with_color = true, bool partial_update = false, bool overwrite = false);
uint32_t read32(SdFile& f);
uint16_t read16(SdFile& f);
void listDir();
void getFileNameFromIndex();
void blinkRed(int flashtimes=3);
void cycleDisplay();
void flagInterrupt();

// total number of files on SD card
int totalFiles = 0;
// current index, persists deep sleep
int currIndex = 0;
// To make sure multiple wakes aren't triggered
volatile bool interruptFlagged = false;
// current file name to load
char currFile[256];

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  if(!SD.begin(SD_CS)) blinkRed(10);
  listDir();
  LowPower.attachInterruptWakeup(WAKE_PIN, flagInterrupt, CHANGE);
  LowPower.attachInterruptWakeup(RTC_ALARM_WAKEUP, flagInterrupt, CHANGE);
}

void cycleDisplay() {
  currIndex = (currIndex+1)%totalFiles; // Will always only cycle between indexes in the range of what's possible.
}

void flagInterrupt() {
  interruptFlagged = true;
}

void loop(void)
{
  digitalWrite(LED_BUILTIN,1);
  if (interruptFlagged) {
    cycleDisplay();
  }
  delay(1000);
  display.init(115200);
  getFileNameFromIndex();
  drawBitmapFromSD(currFile,0,0); // Commented for now for testing wake cycles, let's flash the light some instead
  //blinkRed(20);
  interruptFlagged = false;
  delay(15000);
  display.hibernate();
  digitalWrite(LED_BUILTIN,0);
  LowPower.sleep(DELAY_SEC * 1000);
}

void blinkRed(int flashtimes){
  for (int i=0;i<flashtimes;i++){
    digitalWrite(LED_BUILTIN,1);
    delay(300);
    digitalWrite(LED_BUILTIN,0);
    delay(300);
  }
}

// Builds value of totalFiles on reset
void listDir(){
    File root = SD.open(SLIDESHOW_ROOT);
    if(!root){
        blinkRed(10);
        return;
    }
    if(!root.isDirectory()){
        return;
    }
    File file = root.openNextFile();
    totalFiles = 0;
    while(file){
        if(file.isDirectory()){
          // Do nothing
        } else {
            totalFiles++;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

void getFileNameFromIndex(){
    File root = SD.open(SLIDESHOW_ROOT);
    if(!root){
      blinkRed(5);
        return;
    }
    if(!root.isDirectory()){
        return;
    }
    File file = root.openNextFile();

    int iter = 0;
    while(file){
        if(file.isDirectory()){
          // Do nothing for dir
        } else {
            if(iter == currIndex) {
              strcpy(currFile, SLIDESHOW_ROOT);
              strcat(currFile,"/");
              strcat(currFile, file.name());
              file.close();
              return;
            }
            iter++;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}


static const uint16_t input_buffer_pixels = 20; // may affect performance

static const uint16_t max_row_width = 640; // for up to 7.5" display
static const uint16_t max_palette_pixels = 256; // for depth <= 8

uint8_t input_buffer[3 * input_buffer_pixels]; // up to depth 24
uint8_t output_row_mono_buffer[max_row_width / 8]; // buffer for at least one row of b/w bits
uint8_t output_row_color_buffer[max_row_width / 8]; // buffer for at least one row of color bits
uint8_t mono_palette_buffer[max_palette_pixels / 8]; // palette buffer for depth <= 8 b/w
uint8_t color_palette_buffer[max_palette_pixels / 8]; // palette buffer for depth <= 8 c/w

void drawBitmapFromSD(const char *filename, int16_t x, int16_t y, bool with_color, bool partial_update, bool overwrite)
{
  SdFile file;
  bool valid = false; // valid format to be handled
  bool flip = true; // bitmap is stored bottom-to-top
  uint32_t startTime = millis();
  if ((x >= display.width()) || (y >= display.height())) return;
  file = SD.open(filename);
  if (!file)
  {
    return;
  }
  // Parse BMP header
  if (read16(file) == 0x4D42) // BMP signature
  {
    uint32_t fileSize = read32(file);
    uint32_t creatorBytes = read32(file); // Unused, but done anyway.
    uint32_t imageOffset = read32(file); // Start of image data
    uint32_t headerSize = read32(file);
    uint32_t width  = read32(file);
    uint32_t height = read32(file);
    uint16_t planes = read16(file);
    uint16_t depth = read16(file); // bits per pixel
    uint32_t format = read32(file);
    if ((planes == 1) && ((format == 0) || (format == 3))) // uncompressed is handled, 565 also
    {
      
      // BMP rows are padded (if needed) to 4-byte boundary
      uint32_t rowSize = (width * depth / 8 + 3) & ~3;
      if (depth < 8) rowSize = ((width * depth + 8 - depth) / 8 + 3) & ~3;
      if (height < 0)
      {
        height = -height;
        flip = false;
      }
      uint16_t w = width;
      uint16_t h = height;
      if ((x + w - 1) >= display.width())  w = display.width()  - x;
      if ((y + h - 1) >= display.height()) h = display.height() - y;
      //if (w <= max_row_width) // handle with direct drawing
      {
        valid = true;
        uint8_t bitmask = 0xFF;
        uint8_t bitshift = 8 - depth;
        uint16_t red, green, blue;
        bool whitish, colored;
        if (depth == 1) with_color = false;
        if (depth <= 8)
        {
          if (depth < 8) bitmask >>= depth;
          file.seekSet(54); //palette is always @ 54
          for (uint16_t pn = 0; pn < (1 << depth); pn++)
          {
            blue  = file.read();
            green = file.read();
            red   = file.read();
            file.read();
            whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
            colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
            if (0 == pn % 8) mono_palette_buffer[pn / 8] = 0;
            mono_palette_buffer[pn / 8] |= whitish << pn % 8;
            if (0 == pn % 8) color_palette_buffer[pn / 8] = 0;
            color_palette_buffer[pn / 8] |= colored << pn % 8;
          }
        }
        if (partial_update) display.setPartialWindow(x, y, w, h);
        else display.setFullWindow();
        display.firstPage();
        do
        {
          if (!overwrite) display.fillScreen(GxEPD_WHITE);
          uint32_t rowPosition = flip ? imageOffset + (height - h) * rowSize : imageOffset;
          for (uint16_t row = 0; row < h; row++, rowPosition += rowSize) // for each line
          {
            uint32_t in_remain = rowSize;
            uint32_t in_idx = 0;
            uint32_t in_bytes = 0;
            uint8_t in_byte = 0; // for depth <= 8
            uint8_t in_bits = 0; // for depth <= 8
            uint16_t color = GxEPD_WHITE;
            file.seekSet(rowPosition);
            for (uint16_t col = 0; col < w; col++) // for each pixel
            {
              // Time to read more pixel data?
              if (in_idx >= in_bytes) // ok, exact match for 24bit also (size IS multiple of 3)
              {
                in_bytes = file.read(input_buffer, in_remain > sizeof(input_buffer) ? sizeof(input_buffer) : in_remain);
                in_remain -= in_bytes;
                in_idx = 0;
              }
              switch (depth)
              {
                case 24:
                  blue = input_buffer[in_idx++];
                  green = input_buffer[in_idx++];
                  red = input_buffer[in_idx++];
                  whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
                  colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
                  break;
                case 16:
                  {
                    uint8_t lsb = input_buffer[in_idx++];
                    uint8_t msb = input_buffer[in_idx++];
                    if (format == 0) // 555
                    {
                      blue  = (lsb & 0x1F) << 3;
                      green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                      red   = (msb & 0x7C) << 1;
                    }
                    else // 565
                    {
                      blue  = (lsb & 0x1F) << 3;
                      green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                      red   = (msb & 0xF8);
                    }
                    whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
                    colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0)); // reddish or yellowish?
                  }
                  break;
                case 1:
                case 4:
                case 8:
                  {
                    if (0 == in_bits)
                    {
                      in_byte = input_buffer[in_idx++];
                      in_bits = 8;
                    }
                    uint16_t pn = (in_byte >> bitshift) & bitmask;
                    whitish = mono_palette_buffer[pn / 8] & (0x1 << pn % 8);
                    colored = color_palette_buffer[pn / 8] & (0x1 << pn % 8);
                    in_byte <<= depth;
                    in_bits -= depth;
                  }
                  break;
              }
              if (whitish)
              {
                color = GxEPD_WHITE;
              }
              else if (colored && with_color)
              {
                color = GxEPD_COLORED;
              }
              else
              {
                color = GxEPD_BLACK;
              }
              uint16_t yrow = y + (flip ? h - row - 1 : row);
              display.drawPixel(x + col, yrow, color);
            } // end pixel
          } // end line
        }
        while (display.nextPage());
        delay(1000);
      }
    }
  }
  file.close();
  delay(1000);
  if (!valid)
  {
  }
}

uint16_t read16(SdFile& f)
{
  // BMP data is stored little-endian, same as Arduino.
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(SdFile& f)
{
  // BMP data is stored little-endian, same as Arduino.
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read(); // MSB
  return result;
}
