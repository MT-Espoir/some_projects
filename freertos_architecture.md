# FreeRTOS Architecture Plan for ESP32-CAM RTSP

## 🏗️ **MULTI-TASK ARCHITECTURE**

### **Task Distribution:**

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   Camera Task   │    │   RTSP Task     │    │   Web Task      │
│   Priority: 5   │    │   Priority: 4   │    │   Priority: 3   │
│   Core: 1       │    │   Core: 0       │    │   Core: 0       │
│   Stack: 8KB    │    │   Stack: 6KB    │    │   Stack: 4KB    │
└─────────────────┘    └─────────────────┘    └─────────────────┘
         │                       │                       │
         │                       │                       │
    ┌─────────────────────────────┼───────────────────────┼──────┐
    │                             │                       │      │
    │    ┌─────────────────┐     │    ┌─────────────────┐  │     │
    │    │   WiFi Task     │     │    │  Monitor Task   │  │     │
    │    │   Priority: 3   │     │    │   Priority: 2   │  │     │
    │    │   Core: 0       │     │    │   Core: 1       │  │     │
    │    │   Stack: 4KB    │     │    │   Stack: 2KB    │  │     │
    │    └─────────────────┘     │    └─────────────────┘  │     │
    │                             │                        │     │
    └─────────────────────────────┼────────────────────────┼─────┘
                                  │                        │
                            ┌─────────────────┐           │
                            │   Queue/Mutex   │           │
                            │   Semaphores    │           │
                            │  Event Groups   │           │
                            └─────────────────┘           │
                                                          │
                              ┌─────────────────────────────┘
                              │
                        ┌─────────────────┐
                        │ Memory Pools    │
                        │ DMA Buffers     │
                        │ PSRAM Manager   │
                        └─────────────────┘
```

## 📋 **TASK DETAILS**

### **1. Camera Task (Highest Priority)**
- **Purpose**: Capture frames, manage camera hardware
- **Frequency**: 30 FPS (33ms cycle)
- **Core**: Core 1 (dedicated for camera processing)
- **Responsibilities**:
  - Frame capture from OV5640
  - JPEG compression
  - Frame buffer management
  - Send frames to RTSP queue

### **2. RTSP Task (High Priority)**
- **Purpose**: Handle RTSP streaming protocol
- **Core**: Core 0 (network processing)
- **Responsibilities**:
  - RTSP session management
  - RTP packet construction
  - UDP transmission
  - Client connection handling

### **3. Web Task (Medium Priority)**
- **Purpose**: Web interface and configuration
- **Core**: Core 0 (shared with networking)
- **Responsibilities**:
  - HTTP server operations
  - Configuration management
  - Status page generation
  - Stream endpoint handling

### **4. WiFi Task (Medium Priority)**
- **Purpose**: Network management and monitoring
- **Responsibilities**:
  - WiFi connection management
  - Signal strength monitoring
  - Network error handling
  - Reconnection logic

### **5. Monitor Task (Low Priority)**
- **Purpose**: System health monitoring
- **Core**: Core 1 (background monitoring)
- **Responsibilities**:
  - Memory usage tracking
  - Performance metrics
  - Error logging
  - Watchdog management

## 🔄 **INTER-TASK COMMUNICATION**

### **Queues:**
```c
// Frame data from Camera → RTSP
QueueHandle_t frame_queue;          // Size: 2-3 frames
// Configuration updates
QueueHandle_t config_queue;         // Size: 10 items
// Status/monitoring data
QueueHandle_t status_queue;         // Size: 5 items
```

### **Mutexes:**
```c
// Camera hardware access
SemaphoreHandle_t camera_mutex;
// WiFi status access
SemaphoreHandle_t wifi_mutex;
// Configuration access
SemaphoreHandle_t config_mutex;
```

### **Event Groups:**
```c
// System events
EventGroupHandle_t system_events;
#define WIFI_CONNECTED_BIT    BIT0
#define CAMERA_READY_BIT      BIT1
#define RTSP_ACTIVE_BIT       BIT2
#define CONFIG_CHANGED_BIT    BIT3
```

## ⚡ **PERFORMANCE OPTIMIZATIONS**

### **Memory Management:**
- **PSRAM allocation**: Large buffers (frame buffers)
- **SRAM allocation**: Small, frequent operations
- **DMA buffers**: Direct camera-to-network transfer
- **Memory pools**: Pre-allocated for frame processing

### **CPU Core Utilization:**
- **Core 0**: Network, Web, WiFi tasks
- **Core 1**: Camera, Monitor, heavy processing
- **Load balancing**: Dynamic task migration if needed

### **Real-time Constraints:**
- **Camera task**: Hard real-time (30 FPS deadline)
- **RTSP task**: Soft real-time (network dependent)
- **Monitor task**: Background (no deadline)

## 🛠️ **MIGRATION STRATEGY**

### **Phase 1: Framework Setup**
1. Change `platformio.ini` framework
2. Replace Arduino libraries with ESP-IDF equivalents
3. Setup basic FreeRTOS task structure

### **Phase 2: Task Implementation**
1. Convert `setup()` → task creation in `app_main()`
2. Convert `loop()` → individual task functions
3. Implement inter-task communication

### **Phase 3: Optimization**
1. Fine-tune task priorities and stack sizes
2. Optimize memory allocation
3. Implement error handling and recovery

### **Phase 4: Testing & Validation**
1. Performance benchmarking
2. Stress testing with multiple clients
3. Long-term stability testing
