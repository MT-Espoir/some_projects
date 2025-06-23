#include <Arduino.h>
#include <esp_wifi.h>
#include <soc/rtc_cntl_reg.h>
#include <driver/i2c.h>
#include <IotWebConf.h>
#include <IotWebConfTParameter.h>
#include "esp_camera.h"
#include <ESPmDNS.h>
#include <rtsp_server.h>
// #include <lookup_camera_effect.h>
#include <lookup_camera_frame_size.h>
// #include <lookup_camera_gainceiling.h>
// #include <lookup_camera_wb_mode.h>
#include <format_duration.h>
#include <format_number.h>
#include <moustache.h>
#include <settings.h>

#define OV5640_FRAME_DURATION_MS 33   
#define OV5640_JPEG_QUALITY 20        

// Tối ưu hóa thêm cho 30 FPS
#define OV5640_XCLK_FREQ_HZ 24000000  

// HTML files
extern const char index_html_min_start[] asm("_binary_html_index_min_html_start");

auto param_group_camera = iotwebconf::ParameterGroup("camera", "Camera settings");
auto param_frame_duration = iotwebconf::Builder<iotwebconf::UIntTParameter<unsigned long>>("fd").label("Frame duration (ms)").defaultValue(DEFAULT_FRAME_DURATION).min(10).build();
auto param_frame_size = iotwebconf::Builder<iotwebconf::SelectTParameter<sizeof(frame_sizes[0])>>("fs").label("Frame size").optionValues((const char *)&frame_sizes).optionNames((const char *)&frame_sizes).optionCount(sizeof(frame_sizes) / sizeof(frame_sizes[0])).nameLength(sizeof(frame_sizes[0])).defaultValue(DEFAULT_FRAME_SIZE).build();
auto param_jpg_quality = iotwebconf::Builder<iotwebconf::UIntTParameter<byte>>("q").label("JPG quality").defaultValue(DEFAULT_JPEG_QUALITY).min(1).max(100).build();
auto param_brightness = iotwebconf::Builder<iotwebconf::IntTParameter<int>>("b").label("Brightness").defaultValue(DEFAULT_BRIGHTNESS).min(-2).max(2).build();


// Camera
OV2640 cam;
// DNS Server
DNSServer dnsServer;
// RTSP Server
std::unique_ptr<rtsp_server> camera_server;
// Web server
WebServer web_server(80);

auto thingName = String(WIFI_SSID) + "-" + String(ESP.getEfuseMac(), 16);
IotWebConf iotWebConf(thingName.c_str(), &dnsServer, &web_server, WIFI_PASSWORD, CONFIG_VERSION);

// Camera initialization result
esp_err_t camera_init_result;
bool camera_ready = false;

void handle_root()
{
  log_v("Handle root");
  // Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
    return;

  // Format hostname
  auto hostname = "esp32-" + WiFi.macAddress() + ".local";
  hostname.replace(":", "");
  hostname.toLowerCase();

  // Wifi Modes
  const char *wifi_modes[] = {"NULL", "STA", "AP", "STA+AP"};
  auto ipv4 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP() : WiFi.localIP();
  auto ipv6 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIPv6() : WiFi.localIPv6();

  auto initResult = esp_err_to_name(camera_init_result);
  if (initResult == nullptr)
    initResult = "Unknown reason";

  moustache_variable_t substitutions[] = {
      // Version / CPU
      {"AppTitle", APP_TITLE},
      {"AppVersion", APP_VERSION},
      {"BoardType", BOARD_NAME},
      {"ThingName", iotWebConf.getThingName()},
      {"SDKVersion", ESP.getSdkVersion()},
      {"ChipModel", ESP.getChipModel()},
      {"ChipRevision", String(ESP.getChipRevision())},
      {"CpuFreqMHz", String(ESP.getCpuFreqMHz())},
      {"CpuCores", String(ESP.getChipCores())},
      {"FlashSize", format_memory(ESP.getFlashChipSize(), 0)},
      {"HeapSize", format_memory(ESP.getHeapSize())},
      {"PsRamSize", format_memory(ESP.getPsramSize(), 0)},
      // Diagnostics
      {"Uptime", String(format_duration(millis() / 1000))},
      {"FreeHeap", format_memory(ESP.getFreeHeap())},
      {"MaxAllocHeap", format_memory(ESP.getMaxAllocHeap())},
      {"NumRTSPSessions", camera_server != nullptr ? String(camera_server->num_connected()) : "RTSP server disabled"},
      // Network
      {"HostName", hostname},
      {"MacAddress", WiFi.macAddress()},
      {"AccessPoint", WiFi.SSID()},
      {"SignalStrength", String(WiFi.RSSI())},
      {"WifiMode", wifi_modes[WiFi.getMode()]},
      {"IPv4", ipv4.toString()},
      {"IPv6", ipv6.toString()},
      {"NetworkState.ApMode", String(iotWebConf.getState() == iotwebconf::NetworkState::ApMode)},
      {"NetworkState.OnLine", String(iotWebConf.getState() == iotwebconf::NetworkState::OnLine)},
      // Camera
      {"FrameSize", String(param_frame_size.value())},
      {"FrameDuration", String(param_frame_duration.value())},
      {"FrameFrequency", String(1000.0 / param_frame_duration.value(), 1)},
      {"JpegQuality", String(param_jpg_quality.value())},
      {"CameraInitialized", String(camera_init_result == ESP_OK)},
      {"CameraInitResult", String(camera_init_result)},
      {"CameraInitResultText", initResult},
      // Settings
      // RTSP
      {"RtspPort", String(RTSP_PORT)}};

  web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  auto html = moustache_render(index_html_min_start, substitutions);
  web_server.send(200, "text/html", html);
}

#define STREAM_CONTENT_BOUNDARY "123456789000000000000987654321"

void handle_stream()
{
  log_v("handle_stream");
  if (camera_init_result != ESP_OK || !camera_ready)
  {
    web_server.send(404, "text/plain", "Camera is not initialized or not ready");
    return;
  }

  log_v("starting streaming");
  // Blocks further handling of HTTP server until stopped
  char size_buf[12];
  auto client = web_server.client();
  client.write("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: multipart/x-mixed-replace; boundary=" STREAM_CONTENT_BOUNDARY "\r\n");
  while (client.connected())
  {
    client.write("\r\n--" STREAM_CONTENT_BOUNDARY "\r\n");
    cam.run();
    
    // Add safety checks before accessing cam methods
    auto frame_size = cam.getSize();
    auto frame_buffer = cam.getfb();
    
    if (frame_buffer == nullptr || frame_size == 0)
    {
      log_e("Invalid frame buffer or size, breaking stream");
      break;
    }
    
    client.write("Content-Type: image/jpeg\r\nContent-Length: ");
    sprintf(size_buf, "%d\r\n\r\n", frame_size);
    client.write(size_buf);
    client.write(frame_buffer, frame_size);
  }

  log_v("client disconnected");
  client.stop();
  log_v("stopped streaming");
}

esp_err_t initialize_camera()
{
  log_v("initialize_camera");    
  auto frame_size = FRAMESIZE_HD;  
  log_i("Frame size: %d", frame_size);
  auto jpeg_quality = OV5640_JPEG_QUALITY;
  log_i("JPEG quality: %d", jpeg_quality);
  auto frame_duration = OV5640_FRAME_DURATION_MS; 
  log_i("Frame duration: %d ms ", frame_duration);
  auto brightness = 1;
  log_i("Brightness: %d", brightness);
  auto hmirror = true; // Enable horizontal mirror to fix camera orientation
  log_i("Horizontal mirror: %d", hmirror);
  const camera_config_t camera_config = {
    .pin_pwdn = CAMERA_CONFIG_PIN_PWDN,         // GPIO pin for camera power down line
    .pin_reset = CAMERA_CONFIG_PIN_RESET,       // GPIO pin for camera reset line
    .pin_xclk = CAMERA_CONFIG_PIN_XCLK,         // GPIO pin for camera XCLK line
    .pin_sccb_sda = CAMERA_CONFIG_PIN_SCCB_SDA, // GPIO pin for camera SDA line
    .pin_sccb_scl = CAMERA_CONFIG_PIN_SCCB_SCL, // GPIO pin for camera SCL line
    .pin_d7 = CAMERA_CONFIG_PIN_Y9,             // GPIO pin for camera D7 line
    .pin_d6 = CAMERA_CONFIG_PIN_Y8,             // GPIO pin for camera D6 line
    .pin_d5 = CAMERA_CONFIG_PIN_Y7,             // GPIO pin for camera D5 line
    .pin_d4 = CAMERA_CONFIG_PIN_Y6,             // GPIO pin for camera D4 line
    .pin_d3 = CAMERA_CONFIG_PIN_Y5,             // GPIO pin for camera D3 line
    .pin_d2 = CAMERA_CONFIG_PIN_Y4,             // GPIO pin for camera D2 line
    .pin_d1 = CAMERA_CONFIG_PIN_Y3,             // GPIO pin for camera D1 line
    .pin_d0 = CAMERA_CONFIG_PIN_Y2,             // GPIO pin for camera D0 line
    .pin_vsync = CAMERA_CONFIG_PIN_VSYNC,       // GPIO pin for camera VSYNC line
    .pin_href = CAMERA_CONFIG_PIN_HREF,         // GPIO pin for camera HREF line
    .pin_pclk = CAMERA_CONFIG_PIN_PCLK,         // GPIO pin for camera PCLK line
    .xclk_freq_hz = OV5640_XCLK_FREQ_HZ,        // Tăng từ 20MHz lên 24MHz cho 30 FPS
    .ledc_timer = CAMERA_CONFIG_LEDC_TIMER,     // LEDC timer to be used for generating XCLK
    .ledc_channel = CAMERA_CONFIG_LEDC_CHANNEL, // LEDC channel to be used for generating XCLK
    .pixel_format = PIXFORMAT_JPEG,             // Format of the pixel data: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG
    .frame_size = frame_size,                   // Size of the output image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    .jpeg_quality = jpeg_quality,               // Quality of JPEG output. 0-63 lower means higher quality
    .fb_count = 1,         // Number of frame buffers to be allocated. If more than one, then each frame will be acquired (double speed)
    .fb_location = CAMERA_CONFIG_FB_LOCATION,   // The location where the frame buffer will be allocated
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,            // When buffers should be filled
#if CONFIG_CAMERA_CONVERTER_ENABLED
    conv_mode = CONV_DISABLE, // RGB<->YUV Conversion mode
#endif
    .sccb_i2c_port = SCCB_I2C_PORT // If pin_sccb_sda is -1, use the already configured I2C bus by number
  };

  esp_err_t result = cam.init(camera_config);
  if (result == ESP_OK) {
    // Apply camera settings after successful initialization
    auto camera = esp_camera_sensor_get();
    if (camera != nullptr) {
      camera->set_brightness(camera, brightness);
      camera->set_hmirror(camera, hmirror);
      log_i("Applied camera settings: brightness=%d, hmirror=%d", brightness, hmirror);
    }
    
    // Test the camera by taking a frame to ensure it's working
    log_i("Testing camera functionality...");
    cam.run();
    delay(50); // Give camera time to capture
    auto test_size = cam.getSize();
    auto test_fb = cam.getfb();
    if (test_fb != nullptr && test_size > 0) {
      log_i("Camera test successful - frame size: %d bytes", test_size);
      camera_ready = true;
    } else {
      log_e("Camera test failed - invalid frame buffer");
      camera_ready = false;
      result = ESP_FAIL;
    }
  } else {
    camera_ready = false;
  }
  return result;
}

// Thêm hàm monitoring memory cho OV5640
void log_memory_status() {
  log_d("Memory Status - Free Heap: %d, PSRAM Free: %d, Max Alloc: %d", 
        ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getMaxAllocHeap());
}

void log_wifi_signal_strength() {
  if (WiFi.status() == WL_CONNECTED) {
    int32_t rssi = WiFi.RSSI();
    String signal_quality;
    
    if (rssi >= -50) {
      signal_quality = "Excellent";
    } else if (rssi >= -60) {
      signal_quality = "Good";
    } else if (rssi >= -70) {
      signal_quality = "Fair";
    } else if (rssi >= -80) {
      signal_quality = "Weak";
    } else {
      signal_quality = "Very Weak";
    }
    
    log_i("WiFi Signal - RSSI: %d dBm (%s), SSID: %s", 
          rssi, signal_quality.c_str(), WiFi.SSID().c_str());
    
    if (rssi < -70) {
      log_w("WARNING: WiFi signal is weak (< -70dBm). This may cause UDP packet errors!");
    }
  } else {
    log_e("WiFi not connected!");
  }
}

void start_rtsp_server()
{
  log_v("start_rtsp_server");
  // Sử dụng hardcoded frame duration cho OV5640 - 30 FPS
  camera_server = std::unique_ptr<rtsp_server>(new rtsp_server(cam, OV5640_FRAME_DURATION_MS, RTSP_PORT));
  log_i("RTSP server started", OV5640_FRAME_DURATION_MS);
  MDNS.addService("rtsp", "tcp", RTSP_PORT);
}

void on_connected()
{
  log_v("on_connected");
  
  // Log WiFi signal strength when connected
  log_wifi_signal_strength();
  
  // Start the RTSP Server if initialized
  if (camera_init_result == ESP_OK && camera_ready)
    start_rtsp_server();
  else
    log_e("Not starting RTSP server: camera not initialized or not ready");
}

void setup()
{
  // Disable brownout
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

#ifdef CAMERA_POWER_GPIO
  pinMode(CAMERA_POWER_GPIO, OUTPUT);
  digitalWrite(CAMERA_POWER_GPIO, CAMERA_POWER_ON_LEVEL);
#endif

#ifdef USER_LED_GPIO
  pinMode(USER_LED_GPIO, OUTPUT);
  digitalWrite(USER_LED_GPIO, !USER_LED_ON_LEVEL);
#endif

  Serial.begin(115200);
  Serial.setDebugOutput(false);

#ifdef ARDUINO_USB_CDC_ON_BOOT
  // Delay for USB to connect/settle
  delay(5000);
#endif

  log_i("Core debug level: %d", CORE_DEBUG_LEVEL);
  log_i("CPU Freq: %d Mhz, %d core(s)", getCpuFrequencyMhz(), ESP.getChipCores());
  log_i("Free heap: %d bytes", ESP.getFreeHeap());
  log_i("SDK version: %s", ESP.getSdkVersion());
  log_i("Board: %s", BOARD_NAME);
  log_i("Starting " APP_TITLE "...");
  if (CAMERA_CONFIG_FB_LOCATION == CAMERA_FB_IN_PSRAM && !psramInit())
    log_e("Failed to initialize PSRAM");
  // Kiểm tra PSRAM cho OV5640
  if (psramFound()) {
    log_i("PSRAM found: %d bytes total, %d bytes free", ESP.getPsramSize(), ESP.getFreePsram());
    log_i("PSRAM ready for high-resolution OV5640 operations");
  } else {
    log_w("PSRAM not found - camera performance may be limited");
  }

  // Monitor memory usage for OV5640
  log_i("Internal RAM: %d bytes free, largest block: %d bytes", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  param_group_camera.addItem(&param_frame_duration);
  param_group_camera.addItem(&param_frame_size);
  param_group_camera.addItem(&param_jpg_quality);
  param_group_camera.addItem(&param_brightness);
  iotWebConf.addParameterGroup(&param_group_camera);

  iotWebConf.getApTimeoutParameter()->visible = true;
  // iotWebConf.setConfigSavedCallback(on_config_saved);
  iotWebConf.setWifiConnectionCallback(on_connected);
#ifdef USER_LED_GPIO
  iotWebConf.setStatusPin(USER_LED_GPIO, USER_LED_ON_LEVEL);
#endif
  iotWebConf.init();
  // Try to initialize 3 times
  for (auto i = 0; i < 3; i++)
  {
    log_memory_status(); // Monitor memory before init
    camera_init_result = initialize_camera();
    if (camera_init_result == ESP_OK)
    {
      log_i("Camera initialized successfully on attempt %d", i + 1);
      log_memory_status(); // Monitor memory after successful init
      // update_camera_settings();
      break;
    }    
    esp_camera_deinit();
    camera_ready = false;
    log_e("Failed to initialize camera. Error: 0x%0x. Frame size: %s, frame rate: %d ms, jpeg quality: %d", camera_init_result, param_frame_size.value(), param_frame_duration.value(), param_jpg_quality.value());
    delay(500);
  }

  // Set up required URL handlers on the web server
  web_server.on("/", HTTP_GET, handle_root);
  web_server.on("/config", []
                { iotWebConf.handleConfig(); });
  // Camera stream
  web_server.on("/stream", HTTP_GET, handle_stream);

  web_server.onNotFound([]()
                        { iotWebConf.handleNotFound(); });
}

void loop()
{
  iotWebConf.doLoop();

  // Log WiFi signal strength every 30 seconds
  static unsigned long last_wifi_check = 0;
  if (millis() - last_wifi_check > 3000) {
    log_wifi_signal_strength();
    last_wifi_check = millis();
  }

  if (camera_server)
    camera_server->doLoop();
}
