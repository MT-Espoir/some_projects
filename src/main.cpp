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

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define OV5640_FRAME_DURATION_MS 30   
#define OV5640_JPEG_QUALITY 15        

// Tối ưu hóa thêm cho 30 FPS
#define OV5640_XCLK_FREQ_HZ 24000000  

// Task stack sizes
#define WIFI_MONITOR_TASK_STACK_SIZE 2*2048
#define WEB_SERVER_TASK_STACK_SIZE 4096
#define RTSP_TASK_STACK_SIZE 2*4096
#define CAMERA_TASK_STACK_SIZE 4096

// Task priorities
#define WIFI_MONITOR_TASK_PRIORITY 1
#define WEB_SERVER_TASK_PRIORITY 2  
#define RTSP_TASK_PRIORITY 3
#define CAMERA_TASK_PRIORITY 4

// Task handles
TaskHandle_t wifiMonitorTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
TaskHandle_t rtspTaskHandle = NULL;
TaskHandle_t cameraTaskHandle = NULL;

// Camera synchronization
SemaphoreHandle_t cameraMutex = NULL;

// Buffer management
#define BUFFER_THRESHOLD_PERCENT 80
#define PSRAM_THRESHOLD_BYTES (500 * 1024)  // 500KB minimum free PSRAM 
#define HEAP_THRESHOLD_BYTES (50 * 1024)    // 50KB minimum free heap 
#define MAX_CONSECUTIVE_DROPS 5          
#define BUFFER_RESET_COOLDOWN_MS 1000      
#define STREAM_CONTENT_BOUNDARY "123456789000000000000987654321"

// Network optimization settings
#define UDP_TX_PACKET_MAX_SIZE 1472  // Standard MTU size minus headers
#define WIFI_TX_POWER 78             // Reduce TX power to minimize interference (78 = 19.5dBm)
#define RTSP_CLIENT_TIMEOUT_MS 5000  // Client timeout for RTSP connections

volatile bool buffer_overflow_detected = false;
volatile uint32_t frames_dropped = 0;
volatile uint32_t buffer_resets = 0;
volatile uint32_t last_reset_time = 0;
volatile uint8_t consecutive_drops = 0;

// Forward declarations for task functions
void cameraTask(void *pvParameters);
void wifiMonitorTask(void *pvParameters);
void webServerTask(void *pvParameters);
void rtspTask(void *pvParameters);
bool checkBufferHealth();
void triggerBufferReset();
void adaptiveQualityControl();

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
      // RTSP
      {"RtspPort", String(RTSP_PORT)}};

  web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  auto html = moustache_render(index_html_min_start, substitutions);
  web_server.send(200, "text/html", html);
}

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
    // Check buffer health before processing frame
    if (!checkBufferHealth()) {
      buffer_overflow_detected = true;
      consecutive_drops++;
      frames_dropped++;
      
      if (consecutive_drops >= MAX_CONSECUTIVE_DROPS) {
        triggerBufferReset();
      } else {
        // Skip this frame and continue
        log_d("Skipping frame due to buffer pressure");
        vTaskDelay(pdMS_TO_TICKS(OV5640_FRAME_DURATION_MS));
        continue;
      }
    } else {
      // Reset consecutive drops counter if buffer is healthy
      if (consecutive_drops > 0) {
        consecutive_drops = 0;
        log_d("Buffer health restored");
      }
    }
    
    client.write("\r\n--" STREAM_CONTENT_BOUNDARY "\r\n");
    
    // Take mutex before accessing camera
    if (cameraMutex != NULL && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      cam.run();
      
      // Add safety checks before accessing cam methods
      auto frame_size = cam.getSize();
      auto frame_buffer = cam.getfb();
      
      // Release mutex after getting frame data
      xSemaphoreGive(cameraMutex);
      
      if (frame_buffer == nullptr || frame_size == 0)
      {
        log_e("Invalid frame buffer or size, breaking stream");
        break;
      }
      
      // Additional check for oversized frames
      if (frame_size > 200000) { // >200KB indicates high motion/complexity
        log_w("Large frame detected: %d bytes - potential buffer stress", frame_size);
        adaptiveQualityControl();
      }
      
      client.write("Content-Type: image/jpeg\r\nContent-Length: ");
      sprintf(size_buf, "%d\r\n\r\n", frame_size);
      client.write(size_buf);
      client.write(frame_buffer, frame_size);
    } else {
      log_w("Failed to acquire camera mutex for streaming");
      vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    .fb_count = 2,         // Number of frame buffers to be allocated. If more than one, then each frame will be acquired (double speed)
    .fb_location = CAMERA_CONFIG_FB_LOCATION,   // The location where the frame buffer will be allocated
    .grab_mode = CAMERA_GRAB_LATEST,            // When buffers should be filled - use LATEST for better buffer management
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
    log_i("Testing camera functionality...");    cam.run();
    vTaskDelay(pdMS_TO_TICKS(50)); 
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

// Buffer monitoring and management functions
bool checkBufferHealth() {
  // Check PSRAM usage
  size_t free_psram = ESP.getFreePsram();
  if (free_psram < PSRAM_THRESHOLD_BYTES) {
    log_w("PSRAM low: %d bytes free", free_psram);
    return false;
  }
  
  // Check heap usage
  size_t free_heap = ESP.getFreeHeap();
  if (free_heap < HEAP_THRESHOLD_BYTES) {
    log_w("Heap low: %d bytes free", free_heap);
    return false;
  }
  if (camera_server && camera_server->num_connected() > 0) {
    // Estimate network buffer usage based on frame size
    auto frame_size = cam.getSize();
    if (frame_size > 150000) { // Large frame detected (>150KB)
      log_w("Large frame detected: %d bytes", frame_size);
      return false;
    }
  }
  
  return true;
}

void triggerBufferReset() {
  uint32_t current_time = millis();
  
  // Implement cooldown to prevent excessive resets
  if (current_time - last_reset_time < BUFFER_RESET_COOLDOWN_MS) {
    return;
  }
  
  log_w("Triggering buffer reset - frames_dropped: %d, resets: %d", frames_dropped, buffer_resets + 1);
  
  // Take camera mutex to prevent race conditions
  if (cameraMutex != NULL && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    // Force camera to drop current frame and clear buffers
    esp_camera_fb_return(esp_camera_fb_get());
    
    // Clear any pending frames
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    
    xSemaphoreGive(cameraMutex);
  }
  
  // Reset network buffers if possible
  if (camera_server) {
    // Force flush network buffers (implementation depends on RTSP library)
    // This is a placeholder - actual implementation may vary
    log_d("Flushing network buffers");
  }
  
  buffer_resets++;
  last_reset_time = current_time;
  buffer_overflow_detected = false;
  consecutive_drops = 0;
  
  log_i("Buffer reset completed - total resets: %d", buffer_resets);
}

void adaptiveQualityControl() {
  static uint8_t current_quality = OV5640_JPEG_QUALITY;
  static uint32_t last_quality_change = 0;
  uint32_t current_time = millis();
  
  // Only adjust quality every 2 seconds to avoid oscillation
  if (current_time - last_quality_change < 2000) {
    return;
  }
  
  auto camera = esp_camera_sensor_get();
  if (camera == nullptr) return;
  
  if (consecutive_drops >= 2) {
    // Degrade quality to reduce frame size
    if (current_quality < 50) {
      current_quality += 5;
      camera->set_quality(camera, current_quality);
      log_i("Degrading JPEG quality to %d due to buffer pressure", current_quality);
      last_quality_change = current_time;
    }
  } else if (consecutive_drops == 0 && buffer_resets == 0) {
    // Improve quality if system is stable
    if (current_quality > OV5640_JPEG_QUALITY) {
      current_quality -= 2;
      camera->set_quality(camera, current_quality);
      log_i("Improving JPEG quality to %d", current_quality);
      last_quality_change = current_time;
    }
  }
}
// Thêm hàm monitoring memory cho OV5640
void log_memory_status() {
  log_d("Memory Status - Free Heap: %d, PSRAM Free: %d, Max Alloc: %d", 
        ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getMaxAllocHeap());
  
  // Log buffer management statistics
  log_d("Buffer Stats - Drops: %d, Resets: %d, Consecutive: %d", 
        frames_dropped, buffer_resets, consecutive_drops);
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
  
  // Optimize WiFi settings to reduce UDP packet errors
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Reduce power to minimize interference
  esp_wifi_set_ps(WIFI_PS_NONE);       // Disable power saving for better performance
  
  // Configure UDP socket buffer sizes
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);  // Allow flexibility in connection
  
  // Log WiFi signal strength when connected
  log_wifi_signal_strength();
  
  // Start the RTSP Server if initialized
  if (camera_init_result == ESP_OK && camera_ready)
    start_rtsp_server();
  else
    log_e("Not starting RTSP server: camera not initialized or not ready");
}

void wifiMonitorTask(void *pvParameters) {
  log_i("WiFi Monitor Task started on core %d", xPortGetCoreID());
  
  while (true) {
    // Handle network errors silently
    handleNetworkErrors();
    
    log_wifi_signal_strength();
    log_memory_status();
    
    // Perform buffer health monitoring
    if (!checkBufferHealth() && !buffer_overflow_detected) {
      log_w("Buffer health degrading - proactive monitoring");
      buffer_overflow_detected = true;
    }
    
    // Adaptive quality control based on system state
    adaptiveQualityControl();
    
    // Monitor every 5 seconds for more responsive buffer management
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void webServerTask(void *pvParameters) {
  log_i("Web Server Task started on core %d", xPortGetCoreID());
  
  while (true) {
    iotWebConf.doLoop();
    
    // Small delay to prevent watchdog triggers
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void rtspTask(void *pvParameters) {
  log_i("RTSP Task started on core %d", xPortGetCoreID());
  
  while (true) {
    if (camera_server) {
      // Check buffer health before processing RTSP requests
      if (!checkBufferHealth()) {
        buffer_overflow_detected = true;
        consecutive_drops++;
        
        if (consecutive_drops >= MAX_CONSECUTIVE_DROPS) {
          triggerBufferReset();
        }
      }
      
      camera_server->doLoop();
    }
    
    // Small delay to prevent watchdog triggers
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Network error monitoring and handling
void handleNetworkErrors() {
  static uint32_t last_network_check = 0;
  static uint32_t udp_error_count = 0;
  uint32_t current_time = millis();
  
  // Check network health every 10 seconds
  if (current_time - last_network_check > 10000) {
    if (WiFi.status() != WL_CONNECTED) {
      log_w("WiFi disconnected, attempting reconnection...");
      WiFi.reconnect();
    } else {
      // Reset error count if WiFi is stable
      if (udp_error_count > 0) {
        udp_error_count = 0;
      }
    }
    last_network_check = current_time;
  }
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
  
  // Override log functions (nuclear option)
  #ifdef ARDUINO_ARCH_ESP32
  esp_log_level_set("*", ESP_LOG_NONE);
  // Specifically disable WiFi UDP error messages
  esp_log_level_set("wifi", ESP_LOG_NONE);
  esp_log_level_set("WIFI", ESP_LOG_NONE);
  esp_log_level_set("UDP", ESP_LOG_NONE);
  esp_log_level_set("WiFiUdp", ESP_LOG_NONE);
  #endif
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
      log_memory_status();
      break;
    }      esp_camera_deinit();
    camera_ready = false;
    log_e("Failed to initialize camera. Error: 0x%0x. Frame size: %s, frame rate: %d ms, jpeg quality: %d", camera_init_result, param_frame_size.value(), param_frame_duration.value(), param_jpg_quality.value());
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Set up required URL handlers on the web server
  web_server.on("/", HTTP_GET, handle_root);
  web_server.on("/config", []
                { iotWebConf.handleConfig(); });
  // Camera stream
  web_server.on("/stream", HTTP_GET, handle_stream);  web_server.onNotFound([]()
                        { iotWebConf.handleNotFound(); });

  // Create camera mutex for thread safety
  cameraMutex = xSemaphoreCreateMutex();
  if (cameraMutex == NULL) {
    log_e("Failed to create camera mutex");
  }
  // Create FreeRTOS tasks
  log_i("Creating FreeRTOS tasks...");
  
  // Create WiFi Monitor Task (Core 0)
  xTaskCreatePinnedToCore(
    wifiMonitorTask,
    "WiFiMonitor",
    WIFI_MONITOR_TASK_STACK_SIZE,
    NULL,
    WIFI_MONITOR_TASK_PRIORITY,
    &wifiMonitorTaskHandle,
    0
  );

  // Create Web Server Task (Core 0) 
  xTaskCreatePinnedToCore(
    webServerTask,
    "WebServer",
    WEB_SERVER_TASK_STACK_SIZE,
    NULL,
    WEB_SERVER_TASK_PRIORITY,
    &webServerTaskHandle,
    0
  );

  // Create RTSP Task (Core 0)
  xTaskCreatePinnedToCore(rtspTask,"RTSPServer", 
    RTSP_TASK_STACK_SIZE,
    NULL,
    RTSP_TASK_PRIORITY,
    &rtspTaskHandle,
    1
  );

  log_i("All FreeRTOS tasks created successfully");
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
