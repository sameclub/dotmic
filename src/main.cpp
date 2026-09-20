#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <USB.h>
#include <DotMicAudio.h>
#include <tusb.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WifiPortal.h>
#include <atomic>
#include <math.h>
#include "spectrum.h"
#include <freertos/queue.h>

// S3AI hardware baseline: no UART0 (GPIO43/44 are game buttons).
constexpr uint32_t RATE = 48000;
constexpr uint16_t ORANGE = 0xf940, WHITE = 0xbdf5, DIM = 0x2124;
DotMicAudio audio(RATE, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
USBCDC Console;
Arduino_HWSPI bus(12, 3, 11, 46, -1);
Arduino_ST7789 panel(&bus, 7, 4, false, 240, 240, 0, 0, 0, 0);
Arduino_Canvas canvas(240, 240, nullptr);
i2s_chan_handle_t rx = nullptr;
Preferences prefs;
std::atomic<bool> talking{false}, sleeping{false}, streaming{false}, connected{false};
std::atomic<bool> captureStopped{false};
std::atomic<int> micSlot{0}, gain{4};
std::atomic<int> micMode{2}, rmsLeft{0}, rmsRight{0}, pcmRms{0}; // DotMic always uses AUTO; 2=AUTO
std::atomic<uint32_t> lostBytes{0}, readErrors{0};
std::atomic<uint32_t> spectrumFrames{0};
bool micOK = false, displayOK = false, sdOK = false, sdVerified = false, usbOK = false;
bool diagnostics = false, fullRefresh = true, sleepPending = false, ntpStarted = false;
WifiPortal wifiPortal({"DotMic", "#ff2900", "dotmic-net"});
constexpr uint32_t AUTO_SLEEP_MS = 10000;
uint32_t lastActivity = 0;
uint16_t *previousFrame = nullptr;
struct SpectrumFrame { int16_t samples[Spectrum::Size]; uint32_t session; };
QueueHandle_t spectrumQueue=nullptr;
std::atomic<uint32_t> spectrumSession{0};
Spectrum spectrum;
SpectrumEnvelope spectrumEnvelope;
uint32_t spectrumLastSample=0,spectrumLastDraw=0;
bool spectrumOK=false;
struct Key { const char *name; int pin; bool raw=false, down=false; uint32_t changed=0; };
// keys[] must stay in KeyIndex order; the loop dispatches on these indices.
enum KeyIndex { KEY_AI, KEY_APP, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
                KEY_A, KEY_B, KEY_X, KEY_Y, KEY_SELECT, KEY_START, KEY_COUNT };
Key keys[] = {{"AI",0},{"APP",10},{"UP",42},{"DOWN",45},{"LEFT",21},{"RIGHT",14},
              {"A",1},{"B",43},{"X",44},{"Y",2},{"SELECT",48},{"START",47}};
static_assert(sizeof(keys)/sizeof(keys[0]) == KEY_COUNT, "keys[] out of sync with KeyIndex");
constexpr int DIAG_PAGES = 2;
int diagPage = 0;

void usbEvent(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == ARDUINO_USB_EVENTS) {
        if (id == ARDUINO_USB_STARTED_EVENT || id == ARDUINO_USB_RESUME_EVENT) connected = true;
        if (id == ARDUINO_USB_SUSPEND_EVENT) connected = false;
        if (id == ARDUINO_USB_STOPPED_EVENT) { connected = false; streaming = false; }
    } else if (base == ARDUINO_USB_AUDIO_CARD_EVENTS && id == ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT) {
        auto *event = static_cast<arduino_usb_audio_card_event_data_t *>(data);
        if (event->interface_enable.interface == UAC_INTERFACE_MIC) streaming = event->interface_enable.enable;
    }
}

bool beginMicrophone() {
    if (rx) {
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
        rx = nullptr;
    }
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cfg.dma_desc_num = 8;
    cfg.dma_frame_num = 48; // 1 ms DMA blocks, two 32-bit slots on the wire.
    if (i2s_new_channel(&cfg, nullptr, &rx) != ESP_OK) return false;
    i2s_std_config_t stdcfg = {};
    stdcfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE);
    stdcfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    stdcfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    stdcfg.gpio_cfg.bclk = GPIO_NUM_4;
    stdcfg.gpio_cfg.ws = GPIO_NUM_5;
    stdcfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    stdcfg.gpio_cfg.din = GPIO_NUM_6;
    bool ok=i2s_channel_init_std_mode(rx, &stdcfg) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
    // The unselected I2S microphone slot is tri-stated; avoid a floating input.
    gpio_pulldown_en(GPIO_NUM_6);
    if (!ok) {
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
        rx = nullptr;
    }
    return ok;
}

void captureTask(void *) {
    int32_t raw[96];
    int16_t pcm[48];
    float filteredSamples[96];
    float dc[2]={};
    double energy[2]={};
    int energyFrames=0;
    bool wasOpen = false;
    // Static: keep the 2 KiB analysis copy out of the audio task's stack.
    static SpectrumFrame frame;
    int filled=0;
    uint32_t session=0;
    while (!sleeping.load()) {
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(rx, raw, sizeof(raw), &bytes, 25);
        if (err != ESP_OK || bytes != sizeof(raw)) { ++readErrors; filled=0; continue; }
        // Raw release check closes the gate immediately; opening is debounced.
        bool open = talking.load() && digitalRead(0)==LOW && !sleeping.load();
        if (open != wasOpen) tud_audio_clear_ep_in_ff();
        // Discard the transition block: it may include pre-press DMA samples.
        bool sendVoice = open && wasOpen;
        uint32_t currentSession=spectrumSession.load();
        if(!sendVoice || currentSession!=session) filled=0;
        session=currentSession;
        wasOpen=open;
        int slot=micSlot.load(), multiplier=gain.load();
        double blockEnergy[2]={0,0};
        for (int i=0; i<48; ++i) {
            // MSM261S signed I2S data occupies the high bits of each 32-bit slot.
            float filtered[2];
            for(int channel=0;channel<2;++channel) {
                float sample=static_cast<float>(raw[i*2+channel])/65536.0f;
                dc[channel]+=0.002f*(sample-dc[channel]);
                filtered[channel]=sample-dc[channel];
                filteredSamples[i*2+channel]=filtered[channel];
                blockEnergy[channel]+=filtered[channel]*filtered[channel];
                energy[channel]+=filtered[channel]*filtered[channel];
            }
        }
        // The microphone may be wired to either I2S slot.  AUTO picks the
        // clearly active slot, while a stale saved LEFT/RIGHT choice is also
        // allowed to fall back when it is silent and the other slot is live.
        const int mode=micMode.load();
        if (mode==2) {
            if (blockEnergy[0] > blockEnergy[1]*4.0 && blockEnergy[0] > 1.0e-6) {
                slot=0; micSlot=0;
            } else if (blockEnergy[1] > blockEnergy[0]*4.0 && blockEnergy[1] > 1.0e-6) {
                slot=1; micSlot=1;
            }
        } else {
            const int alternate=slot^1;
            if (blockEnergy[alternate] > 1.0e-6 &&
                (blockEnergy[slot] < 1.0e-8 || blockEnergy[alternate] > blockEnergy[slot]*4.0)) {
                slot=alternate;
                micSlot=slot;
            }
        }
        double pcmEnergy=0;
        for (int i=0; i<48; ++i) {
            int value = sendVoice ? static_cast<int>(filteredSamples[i*2+slot]*multiplier) : 0;
            pcm[i]=static_cast<int16_t>(constrain(value,-32768,32767));
            if (sendVoice) pcmEnergy+=static_cast<double>(pcm[i])*pcm[i];
        }
        if (sendVoice) pcmRms=static_cast<int>(sqrt(pcmEnergy/48.0));
        if(++energyFrames>=200) {
            int left=static_cast<int>(sqrt(energy[0]/(energyFrames*48))*32768.0);
            int right=static_cast<int>(sqrt(energy[1]/(energyFrames*48))*32768.0);
            rmsLeft=left; rmsRight=right;
            if(micMode.load()==2) {
                // Only switch on clear evidence, not near-equal background noise.
                if(left>4 && left>right*4) micSlot=0;
                if(right>4 && right>left*4) micSlot=1;
            }
            energy[0]=energy[1]=0; energyFrames=0;
        }
        if (connected.load() && streaming.load()) {
            // Never block acquisition or replay stale audio to catch up.
            uint16_t sent=audio.write(pcm,sizeof(pcm));
            lostBytes.fetch_add(sizeof(pcm)-sent);
        }
        // Nonblocking, one-frame mailbox: rendering can skip old frames while
        // USB capture continues. The FFT runs in the display loop, not here.
        if(sendVoice && spectrumQueue) for(int i=0;i<48;++i) {
            frame.samples[filled++]=pcm[i];
            if(filled==Spectrum::Size) {
                frame.session=session;
                xQueueOverwrite(spectrumQueue,&frame);
                ++spectrumFrames;
                filled=0;
            }
        }
    }
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    rx = nullptr;
    captureStopped=true;
    vTaskDelete(nullptr);
}

void checkSD() {
    SD_MMC.setPins(40,39,41);
    sdOK=SD_MMC.begin("/sdcard",true,false);
    if (!sdOK) return;
    char path[50]; snprintf(path,sizeof(path),"/dotmic-check-%08lx.tmp",(unsigned long)esp_random());
    if (SD_MMC.exists(path)) return;
    const char expected[]="DotMic SDMMC check";
    File f=SD_MMC.open(path,FILE_WRITE);
    if (!f) return;
    bool written=f.write((const uint8_t*)expected,sizeof(expected))==sizeof(expected);
    f.close(); f=SD_MMC.open(path,FILE_READ);
    char actual[sizeof(expected)]={};
    bool read=f && f.size()==sizeof(expected) && f.readBytes(actual,sizeof(actual))==sizeof(actual);
    f.close(); bool removed=SD_MMC.remove(path);
    sdVerified=written && read && !memcmp(actual,expected,sizeof(expected)) && removed;
    Console.printf("SD path=%s write=%d read=%d delete=%d verify=%d\n",path,written,read,removed,sdVerified);
}

void text(int x,int y,const String &s,uint16_t color=WHITE,int scale=1) {
    canvas.setCursor(x,y); canvas.setTextSize(scale); canvas.setTextColor(color); canvas.print(s);
}
uint16_t blendDot(uint16_t color,float brightness) {
    brightness=constrain(brightness,0.0f,1.0f);
    int r=((DIM>>11)&31)+static_cast<int>((((color>>11)&31)-((DIM>>11)&31))*brightness);
    int g=((DIM>>5)&63)+static_cast<int>((((color>>5)&63)-((DIM>>5)&63))*brightness);
    int b=(DIM&31)+static_cast<int>(((color&31)-(DIM&31))*brightness);
    return (r<<11)|(g<<5)|b;
}
void diagRow(int y,const char *label,const String &value,uint16_t color=WHITE) {
    text(16,y,label,DIM);
    text(70,y,value.substring(0,28),color);
}

void drawDiagStatus() {
    text(16,12,"DOTMIC / STATUS",ORANGE);
    String wifi = wifiPortal.isVerifying()      ? "TESTING NEW NETWORK"
                  : wifiPortal.isProvisioning() ? "AP " + wifiPortal.apName()
                  : wifiPortal.isConnected()    ? wifiPortal.ssid()
                  : wifiPortal.hasCredentials() ? "CONNECTING"
                                                : "NOT CONFIGURED";
    diagRow(34,"WIFI",wifi,wifiPortal.isConnected()?WHITE:ORANGE);
    diagRow(52,"IP",wifiPortal.isConnected()?WiFi.localIP().toString():String("-"));
    diagRow(70,"USB",String(connected.load()?"LINK":"WAIT")+(streaming.load()?"  STREAMING":""),
            connected.load()?WHITE:ORANGE);
    diagRow(88,"MIC",String(micOK?"OK":"FAIL")+"  SLOT "+String(micSlot.load()),micOK?WHITE:ORANGE);
    diagRow(106,"RXERR",String(readErrors.load()),readErrors.load()?ORANGE:WHITE);
    diagRow(124,"DROPS",String(lostBytes.load()),lostBytes.load()?ORANGE:WHITE);
    diagRow(142,"SD",sdOK?(sdVerified?"PASS":"MOUNTED, CHECK FAILED"):String("NOT MOUNTED"),
            sdVerified?WHITE:ORANGE);
    diagRow(160,"FFT",spectrumOK?"OK":"NO MEMORY",spectrumOK?WHITE:ORANGE);
    diagRow(178,"NTP",ntpStarted?"STARTED":"WAITING FOR WIFI");
}

void drawDiagDevice() {
    text(16,12,"DOTMIC / DEVICE",ORANGE);
    diagRow(34,"FW","v1.8");
    diagRow(52,"GAIN",String(gain.load()));
    diagRow(70,"MODE",micMode.load()==2?"AUTO":micMode.load()==0?"LEFT":"RIGHT");
    diagRow(88,"RMS","L "+String(rmsLeft.load())+"  R "+String(rmsRight.load()));
    diagRow(106,"PCM",String(pcmRms.load())+"  FFT "+String((unsigned long)spectrumFrames.load()));
    diagRow(124,"RATE",String(RATE)+" Hz  16 BIT MONO");
    diagRow(142,"HEAP",String(ESP.getFreeHeap()/1024)+"K  PSRAM "+String(ESP.getFreePsram()/1024)+"K");
    diagRow(160,"UPTIME",String(millis()/1000)+"s");
    diagRow(178,"USB ID","303A:D07C");
}

void pageHeader(bool listening) {
    text(18,16,"DOT / MIC",ORANGE);
    const String state=listening?"LISTENING":"CLOCK";
    text((listening?210:222)-state.length()*6,16,state,listening?ORANGE:DIM);
    if(listening) canvas.fillCircle(220,19,2,ORANGE);
}
void pageFooter(const String &caption,bool listening) {
    if (wifiPortal.isProvisioning()) {
        // While the new credentials are tried the hotspot is down, so the screen
        // is the only place the result shows up.
        if (wifiPortal.isVerifying()) {
            text(18,211,"TRYING NEW WIFI",ORANGE);
            text(18,229,"HOTSPOT BACK IF IT FAILS",DIM);
        } else {
            text(18,211,"AP: "+wifiPortal.apName(),ORANGE);
            text(18,229,"PW: "+wifiPortal.apPassword(),ORANGE);
        }
        return;
    }
    String status=!micOK?"MIC ERROR":!usbOK?"USB ERROR":
                  listening && !spectrumOK?"FFT ERROR":!connected?"USB WAIT":
                  listening?(streaming?"USB LIVE":"USB IDLE"):
                  wifiPortal.isConnected()?(ntpStarted?"NTP OK":"WiFi OK"):"USB READY";
    text(18,211,caption,WHITE);
    text(222-status.length()*6,211,status,listening?ORANGE:DIM);
    text(18,229,listening?"RELEASE AI TO MUTE":"HOLD AI TO TALK",DIM);
    String settings="G"+String(gain.load())+" "+(micMode.load()==2?"A":"")+(micSlot.load()?"R":"L");
    text(222-settings.length()*6,229,settings,DIM);
}
const uint8_t digits[10][7] = {
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
 {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},
 {31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
void number(int x,int y,int value,uint16_t color) {
    for(int d=0;d<2;++d) for(int r=0;r<7;++r) for(int c=0;c<5;++c) {
        bool on=value<0 ? r==3 : (digits[d==0 ? value/10 : value%10][r] & (1<<(4-c)));
        canvas.fillCircle(x+d*36+c*6,y+r*6,2,on?color:DIM);
    }
}
void draw() {
    if (!displayOK || sleeping.load()) return;
    canvas.fillScreen(0);
    for(int y=8;y<235;y+=6) for(int x=8;x<235;x+=6) canvas.drawPixel(x,y,0x1082);
    if (diagnostics) {
        if(diagPage==1) drawDiagDevice(); else drawDiagStatus();
        text(16,212,"<> PAGE "+String(diagPage+1)+"/"+String(DIAG_PAGES)+"   SELECT EXIT",DIM);
    } else if (talking.load()) {
        pageHeader(true);
        static SpectrumFrame frame;
        float db[Spectrum::Bands];
        bool fresh=spectrumQueue && xQueueReceive(spectrumQueue,&frame,0)==pdTRUE
                   && frame.session==spectrumSession.load() && digitalRead(0)==LOW;
        uint32_t now=millis();
        if(fresh) {
            spectrum.analyze(frame.samples,db);
            spectrumEnvelope.accept(db); spectrumLastSample=now;
        }
        // A missing display frame is not silence; briefly retain the target.
        if(now-spectrumLastSample>100 || digitalRead(0)!=LOW) spectrumEnvelope.silence();
        spectrumEnvelope.step(now-spectrumLastDraw); spectrumLastDraw=now;
        for(int i=0;i<SpectrumEnvelope::Columns;++i) {
            float h=spectrumEnvelope.column(i);
            for(int j=-12;j<=12;++j) {
                float brightness=constrain(h-abs(j)+1.0f,0.0f,1.0f);
                // A faint connected centerline remains at silence; height is zero.
                if(j==0) brightness=max(0.25f,min(h,1.0f));
                // Match the clock: radius 2, 6 px center-to-center spacing.
                canvas.fillCircle(28+i*6,121+j*6,2,blendDot(ORANGE,brightness));
            }
        }
        pageFooter("LOW > MID > HIGH",true);
    } else {
        time_t now=time(nullptr); tm t={}; localtime_r(&now,&t); bool valid=now>=1704067200;
        pageHeader(false);
        number(28,49,valid?t.tm_hour:-1,WHITE);
        number(28,103,valid?t.tm_min:-1,WHITE);
        number(28,157,valid?t.tm_sec:-1,ORANGE);
        text(16,62,"H",DIM); text(16,116,"M",DIM); text(16,170,"S",DIM);
        // Orange segmented field mirrors the reference, rendered as LED dots.
        for(int x=126;x<222;x+=6) for(int y=49;y<198;y+=6) {
            bool active=valid ? ((y-49)/6 < (t.tm_sec*25/60+1)) : false;
            canvas.fillCircle(x,y,2,active?ORANGE:0x4000);
        }
        char date[28]; strftime(date,sizeof(date),"%a %m.%d",&t);
        String clockCaption = valid ? String(date) : (wifiPortal.hasCredentials() ? "NTP SYNC..." : "START: WIFI");
        pageFooter(clockCaption,false);
    }
    uint16_t *frame=canvas.getFramebuffer();
    for(int y=0;y<240;++y) if(fullRefresh || !previousFrame || memcmp(frame+y*240,previousFrame+y*240,480)) {
        panel.draw16bitRGBBitmap(0,y,frame+y*240,240,1);
        if(previousFrame) memcpy(previousFrame+y*240,frame+y*240,480);
    }
    fullRefresh=false;
}

bool startCaptureTask() {
    captureStopped=false;
    // The previous capture task destroys its I2S channel before sleep. Always
    // create a fresh channel after wake instead of reusing a stale micOK flag.
    micOK=beginMicrophone();
    if (!micOK) return false;
    if (xTaskCreatePinnedToCore(captureTask,"mic",4096,nullptr,4,nullptr,0)!=pdPASS) {
        i2s_channel_disable(rx);
        i2s_del_channel(rx);
        rx=nullptr;
        micOK=false;
        return false;
    }
    return true;
}

void resumeFromSleep() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    for (auto &key : keys) gpio_wakeup_disable(static_cast<gpio_num_t>(key.pin));
    rtc_gpio_hold_dis(GPIO_NUM_9);
    rtc_gpio_deinit(GPIO_NUM_9);
    rtc_gpio_deinit(GPIO_NUM_10);
    pinMode(9,OUTPUT);
    digitalWrite(9,HIGH);
    pinMode(10,INPUT_PULLUP);
    // Adopt the current levels: the key that woke us is not a fresh press.
    for (auto &key : keys) {
        key.raw = key.down = digitalRead(key.pin)==LOW;
        key.changed = millis();
    }
    sleeping=false;
    sleepPending=false;
    spectrumSession.fetch_add(1);
    if (spectrumQueue) xQueueReset(spectrumQueue);
    spectrumEnvelope.reset();
    if (displayOK) panel.displayOn();
    fullRefresh=true;
    startCaptureTask();
    wifiPortal.begin();
    draw();
    lastActivity=millis();
    Console.println("DOTMIC WAKE: APP GPIO10; light sleep resumed");
}

void enterSleep() {
    talking=false; sleeping=true;
    wifiPortal.stopProvisioning();
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    uint32_t start=millis();
    while(micOK && !captureStopped.load() && millis()-start<200) delay(1);
    Console.println("DOTMIC LIGHT SLEEP: APP GPIO10 wakes; RTC time retained");
    if(displayOK) panel.displayOff();
    digitalWrite(9,LOW);
    rtc_gpio_init(GPIO_NUM_9);
    rtc_gpio_set_direction(GPIO_NUM_9,RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_9,0);
    rtc_gpio_hold_en(GPIO_NUM_9);
    SD_MMC.end();
    // AI is a talk key, not a wake key. APP and all navigation/action keys wake
    // the device; after waking, press AI again to start the spectrum.
    esp_err_t err=ESP_OK;
    for (int i=1; i<KEY_COUNT; ++i) {
        auto &key = keys[i];
        err=gpio_wakeup_enable(static_cast<gpio_num_t>(key.pin),GPIO_INTR_LOW_LEVEL);
        if(err!=ESP_OK) break;
    }
    if(err==ESP_OK) err=esp_sleep_enable_gpio_wakeup();
    if(err!=ESP_OK) {
        Console.printf("WAKE CONFIG FAILED %d\n",err);
        // Leave the same way as a normal wake so a transient GPIO setup error
        // cannot strand DotMic with its microphone task stopped and LCD off.
        resumeFromSleep();
        return;
    }
    rtc_gpio_pullup_en(GPIO_NUM_10); rtc_gpio_pulldown_dis(GPIO_NUM_10);
    delay(25);
    Serial.flush();
    int64_t started=esp_timer_get_time();
    err=esp_light_sleep_start();
    Console.printf("DOTMIC LIGHT SLEEP return=%s wake=%d elapsed=%lldms\n",
        esp_err_to_name(err),(int)esp_sleep_get_wakeup_cause(),
        (long long)((esp_timer_get_time()-started)/1000));
    resumeFromSleep();
}

void setup() {
    rtc_gpio_hold_dis(GPIO_NUM_9);
    rtc_gpio_deinit(GPIO_NUM_9);
    rtc_gpio_deinit(GPIO_NUM_10);
    for(auto &key:keys) pinMode(key.pin,INPUT_PULLUP);
    // Consume the wake press so it cannot immediately put us back to sleep.
    while(digitalRead(10)==LOW) delay(10);
    pinMode(9,OUTPUT); digitalWrite(9,LOW);
    setenv("TZ","CST-8",1); tzset();
    prefs.begin("dotmic",false);
    gain=constrain(prefs.getInt("gain",4),1,16);
    // Microphone applications use automatic slot selection.  Migrate any
    // older saved LEFT/RIGHT choice back to AUTO, starting the scan at LEFT.
    const int savedMicMode=constrain(prefs.getInt("slotmode",2),0,2);
    micMode=2;
    micSlot=0;
    if (savedMicMode!=2) prefs.putInt("slotmode",2);
    Console.begin(115200); Console.setTxTimeoutMs(0); Console.enableReboot(false);
    USB.VID(0x303A); USB.PID(0xD07C);
    USB.productName("S3AI DotMic"); USB.manufacturerName("S3AI DIY");
    char usbSerial[32]; snprintf(usbSerial,sizeof(usbSerial),"DOTMIC-%012llX",ESP.getEfuseMac());
    USB.serialNumber(usbSerial); USB.firmwareVersion(0x0080);
    USB.onEvent(usbEvent); audio.onEvent(usbEvent);
    usbOK=audio.begin() && USB.begin();
    displayOK=bus.begin(40000000,SPI_MODE0) && panel.begin(GFX_SKIP_DATABUS_BEGIN) && canvas.begin();
    if(displayOK) {
        // User observed orange rendered blue: select BGR, retain the X mirror.
        bus.beginWrite(); bus.writeC8D8(ST7789_MADCTL,ST7789_MADCTL_MX|0x08); bus.endWrite();
    }
    previousFrame=(uint16_t*)ps_malloc(240*240*2);
    canvas.setTextWrap(false);
    checkSD();
    wifiPortal.begin();
    spectrumQueue=xQueueCreate(1,sizeof(SpectrumFrame));
    spectrumOK=spectrumQueue!=nullptr;
    startCaptureTask();
    lastActivity=millis();
    draw(); digitalWrite(9,HIGH);
}

void loop() {
    uint32_t now=millis();
    for(int i=0;i<KEY_COUNT;++i) {
        auto &key=keys[i]; bool raw=digitalRead(key.pin)==LOW;
        if(raw!=key.raw) { key.raw=raw; key.changed=now; }
        if(key.down!=key.raw && now-key.changed>=25) {
            key.down=key.raw;
            lastActivity=now;
            Console.printf("KEY %s GPIO%d raw=%d %s\n",key.name,key.pin,!raw,key.down?"DOWN":"UP");
            if(i==KEY_AI) {
                ++spectrumSession;
                talking=key.down && !sleepPending;
                spectrumEnvelope.reset();
                spectrumLastSample=spectrumLastDraw=now;
            }
            if(i==KEY_APP && key.down) { sleepPending=true; talking=false; }
            if(key.down && !sleepPending && !talking.load()) {
                if(i==KEY_SELECT) diagnostics=!diagnostics;
                if(diagnostics && (i==KEY_LEFT || i==KEY_RIGHT))
                    diagPage=(diagPage+(i==KEY_RIGHT?1:DIAG_PAGES-1))%DIAG_PAGES;
                if(!diagnostics && (i==KEY_UP || i==KEY_DOWN)) {
                    gain=constrain(gain.load()+(i==KEY_UP?1:-1),1,16);
                    prefs.putInt("gain",gain.load());
                }
                if(i==KEY_START) {
                    if(wifiPortal.isProvisioning()) wifiPortal.stopProvisioning();
                    else wifiPortal.startProvisioning();
                }
            }
        }
        if (key.raw || key.down) lastActivity=now;
    }
    if(!sleeping.load() && !sleepPending && !diagnostics && !talking.load() && !streaming.load() &&
       !wifiPortal.isProvisioning() && now-lastActivity>=AUTO_SLEEP_MS) {
        sleepPending=true;
        talking=false;
        Console.println("DOTMIC AUTO SLEEP: 10s idle");
    }
    bool keysIdle=true;
    for (auto &key : keys) if (key.raw || key.down) { keysIdle=false; break; }
    if(sleepPending && keysIdle) enterSleep();
    wifiPortal.update();
    if (!ntpStarted && wifiPortal.isConnected()) {
        configTzTime("CST-8", "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org");
        ntpStarted = true;
        Console.println("NTP started");
    }
    static uint32_t lastFrame=0,lastLog=0;
    if(now-lastFrame>=25) { lastFrame=now; draw(); }
    if(now-lastLog>=2000) {
        lastLog=now; const auto *p=esp_ota_get_running_partition();
        Console.printf("DOTMIC v1.8 alive part=%s offset=0x%lx size=0x%lx mic=%d usb=%d streaming=%d sd=%d verify=%d drops=%lu rxerr=%lu rmsL=%d rmsR=%d slot=%d mode=%d pcm=%d fft=%lu\n",
          p->label,(unsigned long)p->address,(unsigned long)p->size,micOK,connected.load(),streaming.load(),sdOK,sdVerified,
          (unsigned long)lostBytes.load(),(unsigned long)readErrors.load(),rmsLeft.load(),rmsRight.load(),micSlot.load(),micMode.load(),
          pcmRms.load(),(unsigned long)spectrumFrames.load());
    }
    delay(2);
}
