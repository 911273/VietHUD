# V1 — Thiết bị cảnh báo khoảng cách xe phía trước
## ESP32-S3 + JC3248W535 + HLK-LD2451 + GNSS u-blox M10N

> **Mục đích tài liệu:** đặc tả kỹ thuật và kế hoạch triển khai firmware bằng **Google Antigravity / VS Code + PlatformIO**.
>
> **Phạm vi V1:** thiết bị trợ lái thử nghiệm, dùng radar 24 GHz để phát hiện và theo dõi phương tiện phía trước, tính TTC, hiển thị trực quan trên màn hình cảm ứng và cảnh báo bằng buzzer.
>
> **Không có camera, không nhận diện loại xe, không đọc OBD/CAN.**

---

# 1. Mục tiêu hệ thống

Thiết bị được thiết kế để:

1. Phát hiện phương tiện phía trước bằng radar HLK-LD2451.
2. Hỗ trợ **tối đa 5 mục tiêu**, nhưng tại mỗi thời điểm chỉ hiển thị số mục tiêu thực sự hợp lệ đang được phát hiện: **0–5**.
3. Đo/nhận từ radar:
   - khoảng cách;
   - góc;
   - hướng chuyển động;
   - tốc độ tương đối/radial;
   - SNR và các thông tin phục vụ đánh giá độ tin cậy.
4. Theo dõi mục tiêu theo thời gian, tránh nhấp nháy khi radar mất một vài frame.
5. Xác định quan hệ mục tiêu với hành lang/làn xe:
   - SAME_LANE;
   - ADJACENT_LEFT;
   - ADJACENT_RIGHT;
   - OPPOSITE;
   - UNKNOWN.
6. Tính **TTC — Time To Collision** cho mục tiêu có nguy cơ thực sự.
7. Lấy tốc độ xe chủ từ GNSS M10N 10 Hz.
8. Cảnh báo âm thanh chỉ được phép hoạt động khi tốc độ xe chủ **>60 km/h**.
9. Hiển thị cảnh báo bằng màu sắc và vị trí mục tiêu trên màn hình 3.5".
10. Cho phép thay đổi toàn bộ thông số bằng touchscreen.
11. Lưu cấu hình vào NVS/Preferences của ESP32.
12. Có chế độ giao diện:
    - DAY;
    - NIGHT;
    - AUTO theo GNSS.
13. Không dùng OBD/CAN và không can thiệp vào hệ thống điều khiển xe.

---

# 2. Kiến trúc tổng thể

```text
                         VEHICLE 12 V
                              │
                       Fuse + Protection
                              │
                        DC/DC Buck 12→5 V
                              │
                ┌─────────────┴─────────────┐
                │                           │
                ▼                           ▼
        JC3248W535                    HLK-LD2451
        ESP32-S3                      24 GHz Radar
                │                           │
        ┌───────┼────────┐                  │
        │       │        │                  │
        ▼       ▼        ▼                  │
      LCD     Touch    Buzzer               │
        │                                  UART
        │                                   │
        └───────────────┐       ┌───────────┘
                        ▼       ▼
                     Firmware
                        │
                ┌───────┴────────┐
                │                │
                ▼                ▼
             Tracker          Risk Engine
                │                │
                │                ▼
                │              TTC
                │                │
                └───────┬────────┘
                        │
                        ▼
                  LVGL Dashboard

                  GNSS M10N
                     │
                    UART
                     │
                     ▼
              Ego vehicle speed
```

---

# 3. Phần cứng V1

## 3.1. Main board

**JC3248W535**

- ESP32-S3
- 3.5" LCD
- độ phân giải 320×480
- capacitive touchscreen
- 8 MB PSRAM
- 16 MB Flash
- hỗ trợ LVGL
- USB-C
- các đầu nối JST/HC theo phiên bản board.

### Lưu ý

GPIO của ESP32-S3 là logic 3.3 V. Không đưa tín hiệu logic 5 V trực tiếp vào GPIO.

Không được giả định thứ tự chân vật lý của JST chỉ dựa trên hình ảnh board. Trước khi đấu nguồn/UART thực tế phải xác nhận pinout của đúng revision board bằng tài liệu/đo kiểm.

---

# 4. Radar HLK-LD2451

## 4.1. Chức năng

Radar 24 GHz FMCW, dùng để phát hiện phương tiện và cung cấp thông tin mục tiêu.

Thông số sử dụng trong thiết kế:

- 24–24.25 GHz
- 1T2R
- UART
- logic 3.3 V
- nguồn khoảng 5 V
- khoảng cách cấu hình đến 100 m
- góc ngang khoảng ±15°; tài liệu/marketing có thể ghi khác nhau tùy phiên bản.
- có khả năng trả nhiều mục tiêu.
- Firmware thiết kế tối đa 5 target.

## 4.2. Dữ liệu mục tiêu

Firmware cần parse đầy đủ các trường theo protocol thực tế của module:

```text
Target count
Alarm information
Angle
Distance
Direction
Speed
SNR
...
```

**Không tự suy diễn frame protocol.** Khi lập trình phải dùng đúng protocol version của module đang sử dụng.

## 4.3. Giới hạn thực tế

Mặc dù radar quảng bá khả năng đến khoảng 100 m, không được coi 100 m là khoảng cách chắc chắn trong mọi điều kiện.

Đặc biệt khi đặt radar **sau kính chắn gió**, cần thử nghiệm thực tế vì:

- kính;
- film cách nhiệt/kim loại;
- góc lắp;
- rung;
- vị trí radar;
- hình dạng thân xe;

có thể làm suy giảm tín hiệu 24 GHz.

Mục tiêu thực tế nên ưu tiên vùng khoảng **10–70 m ổn định**, còn 70–100 m là vùng mở rộng cần kiểm chứng.

---

# 5. GNSS u-blox M10N

## 5.1. Mục đích

GNSS chỉ dùng để lấy:

- tốc độ xe chủ;
- thời gian;
- ngày;
- vị trí phục vụ tính sunrise/sunset cho Day/Night Auto.

**GNSS KHÔNG dùng để tính TTC.**

## 5.2. TTC

TTC dùng tốc độ tương đối/radial từ radar:

```text
TTC = Distance / ClosingSpeed
```

Nếu:

```text
ClosingSpeed <= 0
```

thì:

```text
TTC = INFINITY
```

và không có cảnh báo va chạm do TTC.

## 5.3. GNSS speed filtering

Tốc độ GNSS cần lọc:

```text
Raw GNSS speed
       ↓
Median filter
       ↓
EMA / low-pass
       ↓
Ego speed
```

Mục tiêu là tránh bật/tắt cảnh báo âm thanh do tốc độ dao động quanh 60 km/h.

---

# 6. Buzzer

Dùng **active buzzer module 3.3–5 V**, ưu tiên loại có mạch driver/transistor tích hợp.

Nếu buzzer tiêu thụ dòng lớn hoặc là loại bare buzzer không phù hợp GPIO thì phải dùng transistor/MOSFET.

## 6.1. Điều kiện bật âm thanh

Audio warning chỉ được phép khi:

```text
EgoSpeed > 60 km/h
```

Để tránh chatter:

```text
Enable audio:  > 62 km/h
Disable audio: < 58 km/h
```

Khoảng 58–62 km/h là hysteresis.

**Quan trọng:** cảnh báo hình ảnh vẫn hoạt động dưới 60 km/h. Chỉ khóa audio.

## 6.2. Mẫu âm thanh

```text
SAFE      → silent
CAUTION   → beep chậm
WARNING   → beep nhanh
CRITICAL  → continuous
```

Nhịp beep phải được thực hiện non-blocking, không dùng delay().

---

# 7. Target model

Firmware hỗ trợ tối đa 5 target.

```cpp
struct Target {
    bool active;
    uint8_t id;

    float x;
    float y;

    float distance;
    float angle;

    float relativeSpeed;
    float ttc;

    uint8_t lane;
    float confidence;

    uint32_t lastSeen;
};
```

Có thể mở rộng:

```cpp
TargetRelation relation;
uint32_t firstSeen;
uint8_t missedFrames;
float filteredDistance;
float filteredSpeed;
```

---

# 8. Target Relation

```cpp
enum TargetRelation {
    SAME_LANE,
    ADJACENT_LEFT,
    ADJACENT_RIGHT,
    OPPOSITE,
    UNKNOWN
};
```

## 8.1. Xe đi ngược chiều bên trái

Một xe đi ngược chiều ở làn đối diện bên trái:

- vẫn phát hiện;
- vẫn hiển thị;
- không coi là mục tiêu cùng làn;
- không dùng làm primary TTC threat;
- không phát audio TTC chỉ vì xe đó gần.

Phân loại dựa trên tổ hợp:

- góc;
- vị trí ngang;
- hướng chuyển động;
- radial speed;
- lịch sử target;
- lane/corridor;
- confidence.

Nếu chưa đủ chắc chắn:

```text
UNKNOWN
```

thì:

- hiển thị target;
- không dùng làm primary same-lane audio threat.

---

# 9. Target tracking

Không được chỉ lấy target gần nhất làm mục tiêu cảnh báo.

## 9.1. Persistence

Khuyến nghị:

```text
New target:
  cần 2–3 frame liên tiếp
  trước khi coi là target ổn định.

Mất 1 frame:
  giữ target.

Mất khoảng 500 ms:
  xóa target.
```

Các giá trị này phải là configurable/tunable.

## 9.2. Tracking

Tracker cần:

1. match target frame hiện tại với target cũ;
2. giữ ID ổn định;
3. lọc distance;
4. lọc relative speed;
5. lọc angle;
6. tính confidence;
7. xử lý target xuất hiện/mất;
8. hỗ trợ tối đa 5 target.

Không cần Machine Learning cho V1.

---

# 10. Lane model

Không dùng cách chia cứng đơn giản:

```text
LEFT | CENTER | RIGHT
```

mà dùng **dynamic lane corridor**.

Ví dụ:

```text
        LEFT        EGO         RIGHT
         │           │            │
         │     ┌─────┴─────┐      │
         │     │   ego     │      │
         │     │ corridor  │      │
         │     └───────────┘      │
```

Mỗi target được xác định theo:

- lateral position;
- longitudinal position;
- góc;
- lane width;
- ego lane position;
- corridor width;
- hướng chuyển động.

Các thông số này phải điều chỉnh được từ Settings.

### Future

Có thể bổ sung IMU (BMI270/ICM-42688-P) trong phiên bản sau để cải thiện khi xe:

- vào cua;
- đổi làn;
- tăng/giảm tốc;
- đi trên đường cong.

**Không bắt buộc V1.**

---

# 11. Primary target selection

Không chọn:

```text
nearest target
```

một cách đơn giản.

Primary target nên là target thỏa mãn nhiều điều kiện:

```text
SAME_LANE
+
same direction
+
closing
+
confidence cao
+
persistence đủ
```

Có thể tính priority:

```text
priority =
    TTC risk
    + distance risk
    + closing speed
    + lane relevance
    + confidence
```

Mục tiêu có TTC thấp nhất trong hành lang nguy hiểm được ưu tiên.

---

# 12. TTC và Risk Engine

## 12.1. TTC

```text
if closingSpeed <= 0:
    TTC = INF
else:
    TTC = distance / closingSpeed
```

Cần thống nhất đơn vị trong code, ví dụ:

```text
distance      = meter
closingSpeed  = meter/second
TTC           = second
```

## 12.2. Ngưỡng ban đầu

```text
TTC > 5 s       GREEN
3–5 s           YELLOW
1.5–3 s         ORANGE
< 1.5 s         RED
```

Đây chỉ là giá trị khởi đầu để tuning, **không phải giá trị an toàn được chứng nhận**.

Các ngưỡng phải chỉnh được trên touchscreen.

## 12.3. Risk engine

Không dựa duy nhất vào distance.

Risk score có thể sử dụng:

```text
TTC
Distance
Closing speed
Ego speed
Lane relation
Confidence
Target persistence
Direction
```

V1 nên dùng **rule-based**, chưa cần AI/ML.

---

# 13. Audio safety gate

Logic:

```text
if egoSpeed > audioEnableSpeed
    AND target is valid
    AND relation == SAME_LANE
    AND closingSpeed > 0
    AND confidence >= minConfidence
    AND TTC < warningThreshold:

        allow audio
else:
        mute audio
```

Hysteresis:

```text
audioEnableSpeed  = 62 km/h
audioDisableSpeed = 58 km/h
```

Có thể cho phép người dùng cấu hình:

```text
Audio speed threshold = 60 km/h
Hysteresis = 2 km/h
```

---

# 14. UI / Dashboard

Màn hình chính cần đọc được trong khoảng 0.5–1 giây.

## 14.1. Bố cục đề xuất

```text
┌────────────────────────────────┐
│ GPS ●     72 km/h       ⚙     │
│                                │
│            ROAD                │
│                                │
│       ○                        │
│              🚗                │
│                                │
│                 ○              │
│                                │
│          🚘                    │
│                                │
│                                │
│ TTC  2.4 s     TARGETS: 3      │
│                                │
│ Radar ●   GNSS ●   AUDIO ●     │
└────────────────────────────────┘
```

Mục tiêu được:

- đặt theo góc;
- đặt theo khoảng cách;
- animate mượt;
- thay đổi màu theo risk;
- hiển thị số target thực tế 0–5.

## 14.2. Màu mục tiêu

Ví dụ:

```text
GREEN   → safe
YELLOW  → caution
ORANGE  → warning
RED     → critical
GRAY/BLUE → opposite/unknown
```

Không được dùng màu thay cho logic an toàn; màu chỉ là lớp hiển thị.

---

# 15. Day / Night UI

## 15.1. Chế độ

```text
DAY
NIGHT
AUTO
```

## 15.2. V1 Auto

Không cần cảm biến ánh sáng.

Dùng:

```text
GNSS date
GNSS time
GNSS latitude
GNSS longitude
       ↓
Sunrise / Sunset calculation
       ↓
DAY / NIGHT
```

Có thể thêm khoảng đệm:

```text
Sunset  + 5 min → Night
Sunrise + 5 min → Day
```

## 15.3. Độ sáng

Mặc định:

```text
Day   = 100%
Night = 20–30%
```

Chuyển mềm:

```text
1–2 seconds
```

## 15.4. Khi mất GNSS

Không tự đoán thời gian.

Giữ chế độ gần nhất đã xác định và hiển thị trạng thái GNSS.

---

# 16. Settings

Tất cả cấu hình phải chỉnh được bằng touchscreen.

## 16.1. Radar Settings

```text
Maximum range
Minimum target speed
Direction
Target limit
SNR / sensitivity
Trigger count
No-target delay
Apply
Restore default
```

## 16.2. Safety Settings

```text
Audio enable speed
Speed hysteresis
TTC warning threshold
TTC critical threshold
Minimum distance
Minimum confidence
Restore default
```

## 16.3. Audio

```text
Audio ON/OFF
Beep pattern
Warning pattern
Critical pattern
Test buzzer
```

Nếu dùng buzzer đơn giản thì "volume" không nhất thiết có tác dụng. Nếu chuyển sang speaker/amplifier trong tương lai mới cần volume.

## 16.4. Lane

```text
2 / 3 lanes
Ego lane position
Lane width
Detection corridor
Calibration
```

## 16.5. Display

```text
Mode:
  Auto
  Day
  Night

Day brightness
Night brightness
Transition time

Show target speed
Show TTC
Show angle
Show ID
Animation
UI scale
```

## 16.6. GNSS

```text
Fix status
Satellites
Current speed
Update rate
Speed filter
Test GNSS
```

## 16.7. Diagnostics

```text
ESP32 status
PSRAM
LCD
Touch
Radar
Target count
GNSS
Buzzer
FPS
CPU usage
RAM usage
```

---

# 17. Basic / Advanced Settings

Để tránh người lái vô tình thay đổi thông số an toàn:

## Basic

```text
Day/Night
Brightness
Audio ON/OFF
Display options
```

## Advanced

Có thể mở bằng long-press hoặc màn hình xác nhận:

```text
Radar parameters
TTC thresholds
Confidence
Lane model
Filtering
Sensitivity
```

---

# 18. NVS / Preferences

Mọi cấu hình phải lưu persistent:

```text
ESP32 NVS / Preferences
```

Khi boot:

```text
loadConfig()
    ↓
validateConfig()
    ↓
if invalid:
    loadDefaults()
    ↓
configureRadar()
```

Khi người dùng nhấn Apply/Save:

```text
UI
 ↓
validate
 ↓
save NVS
 ↓
apply runtime
 ↓
configure radar if necessary
```

---

# 19. Pin / UART mapping dự kiến

Đây là **logical mapping dự kiến**, không phải xác nhận cuối cùng về thứ tự chân vật lý của từng JST trên mọi revision JC3248W535.

## LCD

Các project thực tế của JC3248W535 sử dụng:

```text
LCD CS    GPIO45
LCD CLK   GPIO47
LCD D0    GPIO21
LCD D1    GPIO48
LCD D2    GPIO40
LCD D3    GPIO39
LCD BL    GPIO1
```

## Touch

```text
SDA  GPIO4
SCL  GPIO8
INT  GPIO3
```

Một số driver có thể không cần INT.

## UART radar

Mapping firmware đề xuất:

```text
UART1 RX = GPIO6
UART1 TX = GPIO7
```

## UART GNSS

Mapping firmware đề xuất:

```text
UART2 RX = GPIO18
UART2 TX = GPIO17
```

TX/RX phải đấu chéo:

```text
Device TX → ESP RX
Device RX → ESP TX
```

## Buzzer

Dự kiến:

```text
GPIO15
```

nhưng phải xác nhận GPIO15 không bị firmware/board revision sử dụng cho chức năng khác.

## Không sử dụng tùy tiện

Tránh các GPIO đang được dùng bởi:

- LCD;
- touch;
- PSRAM;
- JTAG;
- boot;
- các chức năng đặc biệt của board.

Đặc biệt cần thận trọng với GPIO35–37 trên ESP32-S3 board có octal PSRAM và GPIO39–42 nếu đang dùng cho chức năng khác.

---

# 20. Nguồn điện trên ô tô

Không cấp 12 V trực tiếp cho JC3248W535 hoặc radar.

Kiến trúc:

```text
Vehicle 12 V
    │
   Fuse ~1 A
    │
Reverse polarity protection
    │
TVS / transient protection
    │
12→5 V Buck
    │
    ├── JC3248W535
    ├── LD2451
    └── buzzer (nếu phù hợp)
```

Buck nên có khả năng tải thực tế khoảng **≥2–3 A** để có dự phòng.

Khi phát triển trên bàn:

```text
USB-C 5 V
```

để giảm rủi ro.

---

# 21. Phần mềm

## 21.1. Công cụ

Khuyến nghị:

```text
Google Antigravity
+
VS Code-compatible workflow
+
PlatformIO
+
Arduino framework
```

Không ưu tiên Arduino IDE cho project V1 vì project có nhiều module.

## 21.2. Cấu trúc project

```text
radar_car/
│
├── platformio.ini
│
├── src/
│   ├── main.cpp
│   │
│   ├── radar/
│   │   ├── LD2451.cpp
│   │   ├── LD2451.h
│   │   └── protocol.cpp
│   │
│   ├── gnss/
│   │   ├── GNSS.cpp
│   │   └── GNSS.h
│   │
│   ├── tracking/
│   │   ├── TargetTracker.cpp
│   │   └── TargetTracker.h
│   │
│   ├── lane/
│   │   ├── LaneModel.cpp
│   │   └── LaneModel.h
│   │
│   ├── safety/
│   │   ├── TTC.cpp
│   │   ├── RiskEngine.cpp
│   │   └── RiskEngine.h
│   │
│   ├── audio/
│   │   ├── Buzzer.cpp
│   │   └── Buzzer.h
│   │
│   ├── config/
│   │   ├── Config.cpp
│   │   └── Config.h
│   │
│   └── ui/
│       ├── UI.cpp
│       ├── Dashboard.cpp
│       ├── Settings.cpp
│       └── Theme.cpp
│
└── README.md
```

---

# 22. Task / update rates

Không dùng `delay()` trong logic chính.

Đề xuất:

```text
Radar UART receive      non-blocking
Radar processing        20–50 Hz
GNSS                    10 Hz
Target tracker          20 Hz
TTC / Risk              20 Hz
UI                      20–30 Hz
Buzzer                  event/timer based
```

Có thể dùng:

- millis();
- FreeRTOS task;
- queue;
- ring buffer;
- event/state machine.

---

# 23. Nguyên tắc lập trình

## Không block

Không viết:

```cpp
delay(1000);
```

trong loop chính.

## Không phụ thuộc timing giả

Không giả định radar luôn trả frame đều nhau.

## Parser phải chống lỗi

Radar parser cần:

- tìm header;
- kiểm tra frame length;
- validate;
- checksum nếu protocol có;
- bỏ frame lỗi;
- resync sau frame hỏng.

## Fail-safe

Nếu radar/GNSS lỗi:

```text
Radar mất:
    không tạo target giả
    không tạo TTC giả

GNSS mất:
    không giả định ego speed
    disable audio gate dựa trên speed
    hiển thị GNSS fault
```

---

# 24. Radar configuration

Khi khởi động:

```text
load saved radar configuration
        ↓
send configuration to LD2451
        ↓
verify response
        ↓
start monitoring
```

Các tham số ban đầu đề xuất:

```text
Max distance: 100 m
Direction: test both / approaching
Min target speed: khoảng 5 km/h
No-target delay: 1–2 s
Sensitivity/SNR: tune experimentally
```

Không khóa direction quá sớm vì hệ thống vẫn muốn hiển thị mục tiêu bên cạnh/đối diện.

**Quan trọng:** radar speed threshold là thành phần radial và chịu ảnh hưởng bởi góc. Không coi nó là tốc độ đường thực tế của xe.

---

# 25. Luồng dữ liệu chính

```text
LD2451 UART
    ↓
Radar Parser
    ↓
Raw Targets
    ↓
Target Tracker
    ↓
Filtered Targets
    ↓
Lane Model
    ↓
Target Relation
    ↓
Risk Engine
    ↓
TTC / Severity
    ├──────────────┐
    ▼              ▼
 Dashboard       Buzzer
```

Song song:

```text
GNSS UART
   ↓
GNSS Parser
   ↓
Ego Speed + Time + Position
   ├──────────────┐
   ▼              ▼
Audio Gate      Day/Night
```

---

# 26. State machine đề xuất

## System state

```cpp
enum SystemState {
    BOOT,
    SELF_TEST,
    WAIT_RADAR,
    WAIT_GNSS,
    RUNNING,
    DEGRADED,
    ERROR
};
```

## Target state

```cpp
enum TargetState {
    NEW,
    TRACKING,
    LOST,
    DELETED
};
```

## Risk state

```cpp
enum RiskLevel {
    SAFE,
    CAUTION,
    WARNING,
    CRITICAL
};
```

---

# 27. Audio state machine

```text
SAFE
  ↓
CAUTION
  ↓
WARNING
  ↓
CRITICAL
```

Ví dụ:

```text
SAFE:
    silent

CAUTION:
    beep 1 Hz

WARNING:
    beep 2–4 Hz

CRITICAL:
    continuous
```

Chỉ chạy audio nếu:

```text
egoSpeed >= audio enable threshold
```

và target đủ điều kiện.

---

# 28. Dashboard target rendering

Mỗi frame:

```text
for each active target:
    calculate screen position
    interpolate position
    calculate icon scale
    choose color
    draw target
```

Không để target nhảy mạnh giữa các frame.

Có thể dùng:

```text
linear interpolation
```

hoặc low-pass filter.

---

# 29. Hiển thị số target

Không hiển thị cố định 3 hoặc 5.

Phải hiển thị:

```text
TARGETS: 0
TARGETS: 1
TARGETS: 2
...
TARGETS: 5
```

tùy số target đang active và vượt qua validation.

---

# 30. Chức năng không làm trong V1

Không triển khai:

- nhận diện xe máy/ô tô/xe tải;
- camera AI;
- OBD;
- CAN;
- điều khiển phanh;
- điều khiển ga;
- điều khiển hệ thống xe;
- autopilot;
- ML/DL;
- RTK;
- sensor fusion camera;
- IMU bắt buộc.

---

# 31. Roadmap phát triển

## Phase 1 — Board bring-up

1. PlatformIO build.
2. Flash ESP32.
3. USB serial log.
4. LCD.
5. Touch.
6. LVGL basic screen.

## Phase 2 — Radar

1. UART.
2. Raw frame dump.
3. Protocol parser.
4. Display raw target.
5. Multi-target.
6. Max 5 target.

## Phase 3 — GNSS

1. UART.
2. NMEA parser/library.
3. Fix status.
4. Speed.
5. 10 Hz.
6. Filter.

## Phase 4 — Tracker

1. Target ID.
2. Persistence.
3. Timeout.
4. Filtering.
5. Confidence.

## Phase 5 — Lane

1. x/y conversion.
2. Lane corridor.
3. Same lane.
4. Adjacent.
5. Opposite.
6. Unknown.

## Phase 6 — Safety

1. Relative speed.
2. TTC.
3. Risk.
4. Primary target.
5. Audio gate.

## Phase 7 — Buzzer

1. GPIO.
2. Test.
3. Patterns.
4. Speed gate.
5. Hysteresis.

## Phase 8 — Settings

1. LVGL settings pages.
2. Config model.
3. Validation.
4. NVS.
5. Radar apply.

## Phase 9 — Day/Night

1. GNSS time.
2. Position.
3. Sunrise/sunset.
4. Auto.
5. Day theme.
6. Night theme.
7. Brightness transition.

## Phase 10 — Diagnostics

1. FPS.
2. CPU.
3. RAM.
4. PSRAM.
5. Radar status.
6. GNSS status.
7. Buzzer test.

## Phase 11 — Road test

Test:

- xe phía trước cùng làn;
- xe phía trước phanh;
- xe bên trái;
- xe bên phải;
- xe đi ngược chiều;
- nhiều mục tiêu;
- mục tiêu mất/tái xuất hiện;
- tốc độ <60;
- tốc độ >60;
- tốc độ quanh 60;
- ban ngày;
- ban đêm;
- mất GNSS;
- mất radar;
- kính chắn gió thực tế.

---

# 32. Test cases bắt buộc

## T01 — Không có target

Expected:

```text
TARGETS = 0
No TTC
No buzzer
```

## T02 — Một target cùng làn, không closing

```text
TTC = INF
No warning
```

## T03 — Một target cùng làn, closing

```text
TTC decreases
Risk increases
```

## T04 — Xe ngược chiều bên trái

```text
Relation = OPPOSITE
Display target
No same-lane TTC audio
```

## T05 — Unknown relation

```text
Display
No primary audio warning
```

## T06 — 5 target

```text
TARGETS = 5
All valid targets tracked
```

## T07 — Target thứ 6

```text
Do not overflow memory
Keep max 5
```

## T08 — Ego speed 55 km/h

```text
Visual warning: ON
Audio: OFF
```

## T09 — Ego speed 65 km/h

```text
Audio can activate
if risk conditions are met
```

## T10 — Speed oscillates 59–61

```text
No audio chatter
```

## T11 — Speed crosses 62

```text
Audio permission ON
```

## T12 — Speed falls below 58

```text
Audio permission OFF
```

## T13 — GNSS lost

```text
No fake speed
Audio disabled
Status shown
```

## T14 — Radar lost

```text
No fake target
No fake TTC
Radar fault shown
```

## T15 — Day/Night

```text
Auto follows GNSS sunrise/sunset
```

---

# 33. Configuration object

Có thể bắt đầu bằng:

```cpp
struct AppConfig {
    // Radar
    float maxRangeM;
    float minTargetSpeedKmh;
    uint8_t maxTargets;
    uint8_t radarDirection;
    uint8_t snrThreshold;
    uint8_t triggerCount;
    uint16_t noTargetDelayMs;

    // Safety
    float audioEnableSpeedKmh;
    float audioDisableSpeedKmh;
    float ttcWarningSec;
    float ttcCriticalSec;
    float minDistanceM;
    float minConfidence;

    // Lane
    uint8_t laneCount;
    float laneWidthM;
    float egoLanePosition;
    float corridorWidthM;

    // Audio
    bool audioEnabled;
    uint8_t warningPattern;
    uint8_t criticalPattern;

    // Display
    uint8_t displayMode; // AUTO/DAY/NIGHT
    uint8_t dayBrightness;
    uint8_t nightBrightness;
    uint16_t transitionMs;
    bool showSpeed;
    bool showTtc;
    bool showAngle;
    bool showId;
    bool animation;

    // GNSS
    uint8_t gnssRateHz;
    uint8_t speedFilter;
};
```

---

# 34. Default configuration

Đề xuất ban đầu:

```text
maxRangeM              = 100
minTargetSpeedKmh      = 5
maxTargets             = 5

audioEnableSpeedKmh    = 62
audioDisableSpeedKmh   = 58

ttcWarningSec          = 3.0
ttcCriticalSec         = 1.5

minDistanceM           = 3
minConfidence          = tune

laneCount              = 3
laneWidthM             = 3.2–3.6
corridorWidthM         = tune

displayMode            = AUTO
dayBrightness          = 100
nightBrightness        = 25
transitionMs            = 2000

gnssRateHz             = 10
```

Các giá trị cần tune thực nghiệm trước khi dùng thực tế.

---

# 35. Logging / debug

Có các level:

```cpp
LOG_ERROR
LOG_WARN
LOG_INFO
LOG_DEBUG
LOG_TRACE
```

Ví dụ:

```text
[RADAR] targets=3
[TARGET] id=1 d=32.4m a=-2.1 vrel=-8.2
[TRACK] id=1 confidence=0.91
[LANE] id=1 relation=SAME_LANE
[TTC] id=1 ttc=3.95
[RISK] WARNING
[AUDIO] ENABLE
```

Không log quá nhiều khi production vì ảnh hưởng CPU/UART.

---

# 36. Antigravity / VS Code workflow

Antigravity được dùng như coding agent.

## Bước 1

Mở project:

```text
radar_car/
```

## Bước 2

Yêu cầu agent:

```text
Read the project specification in README.md.
Do not invent hardware pin mappings.
Implement the project incrementally.
After each phase, compile and report errors.
Do not proceed to the next phase until the current phase builds.
```

## Bước 3

Triển khai từng phase.

Không yêu cầu agent viết toàn bộ firmware một lần.

---

# 37. Prompt khởi tạo cho Antigravity

Dùng prompt sau:

```text
You are implementing a PlatformIO Arduino project for an ESP32-S3
JC3248W535 board.

Read README.md completely before modifying code.

Project goal:
Build a forward vehicle-distance warning/display prototype using:

- JC3248W535 ESP32-S3 3.5" 320x480 touchscreen
- HLK-LD2451 24 GHz radar
- u-blox M10N GNSS
- active buzzer

Important requirements:

1. Maximum 5 tracked targets.
2. Display the actual number of valid targets from 0 to 5.
3. Do not classify vehicle type.
4. Do not use camera.
5. Do not use OBD or CAN.
6. GNSS provides ego speed only.
7. TTC uses radar relative/closing speed.
8. Opposite-lane targets must not be treated as same-lane collision threats.
9. UNKNOWN relation must be displayed but must not trigger primary TTC audio.
10. Audio is allowed only above 60 km/h, using hysteresis:
    enable above 62 km/h
    disable below 58 km/h.
11. Visual warnings remain active below 60 km/h.
12. All user settings must be configurable through touchscreen.
13. Save settings in ESP32 NVS/Preferences.
14. Day/Night/Auto display modes.
15. Auto Day/Night uses GNSS time and position to calculate sunrise/sunset.
16. No blocking delay() in main logic.
17. Use non-blocking UART parsing.
18. Use LVGL for UI.
19. Build robustly with PlatformIO.
20. Do not invent LD2451 protocol fields. Use the actual protocol implementation/documentation.
21. Do not assume physical JST connector pin order. Logical GPIO mapping must be verified before hardware connection.

Development order:

Phase 1:
LCD + touch + LVGL.

Phase 2:
LD2451 UART + protocol parser.

Phase 3:
GNSS.

Phase 4:
Target tracker.

Phase 5:
Lane model.

Phase 6:
TTC/risk engine.

Phase 7:
Buzzer.

Phase 8:
Settings/NVS.

Phase 9:
Day/Night.

Phase 10:
Diagnostics.

Phase 11:
Road-test tuning.

After every phase:
- compile;
- fix compile errors;
- explain changed files;
- do not silently modify unrelated modules.
```

---

# 38. Definition of Done — V1

V1 được coi là hoàn thành khi:

- [ ] ESP32 boot ổn định.
- [ ] LCD hiển thị ổn định.
- [ ] Touch hoạt động.
- [ ] LVGL dashboard hoạt động.
- [ ] LD2451 parse được frame thực.
- [ ] Phát hiện 0–5 target.
- [ ] Tracking không nhấp nháy.
- [ ] Có relation SAME_LANE.
- [ ] Có OPPOSITE.
- [ ] Có UNKNOWN.
- [ ] TTC hoạt động.
- [ ] Primary target được lựa chọn đúng logic.
- [ ] GNSS cung cấp tốc độ.
- [ ] Audio gate >60 km/h hoạt động.
- [ ] Hysteresis 62/58 hoạt động.
- [ ] Buzzer hoạt động.
- [ ] Settings hoạt động.
- [ ] NVS hoạt động.
- [ ] Day/Night hoạt động.
- [ ] Auto Day/Night hoạt động với GNSS.
- [ ] Diagnostics hoạt động.
- [ ] Không dùng OBD/CAN.
- [ ] Không dùng camera.
- [ ] Không nhận diện loại xe.
- [ ] Không có memory overflow khi radar trả >5 target.
- [ ] Không có blocking delay trong runtime.
- [ ] Có xử lý mất radar/GNSS.
- [ ] Đã road-test qua các tình huống bắt buộc.

---

# 39. Lưu ý an toàn

Đây là **prototype driver-assistance**, không phải hệ thống ADAS được chứng nhận.

Không được sử dụng kết quả radar/TTC như một cơ chế duy nhất để quyết định phanh hoặc điều khiển xe.

Các yếu tố như:

- radar reflection;
- thời tiết;
- kính chắn gió;
- góc lắp;
- mục tiêu nhỏ;
- xe cắt ngang;
- xe đi ngược chiều;
- nhiều mục tiêu;
- đường cong;
- thay đổi làn;
- GNSS mất tín hiệu;

có thể làm kết quả sai.

V1 chỉ nên:

```text
DETECT
TRACK
DISPLAY
WARN
```

Không:

```text
CONTROL VEHICLE
```

---

# 40. Quyết định thiết kế đã chốt

```text
MAIN BOARD
    JC3248W535 ESP32-S3

RADAR
    HLK-LD2451 24 GHz

GNSS
    u-blox M10N 10 Hz

DISPLAY
    3.5" 320x480 capacitive touchscreen

UI
    LVGL

AUDIO
    Active buzzer

TARGETS
    0–5 actual valid targets

VEHICLE CLASSIFICATION
    NONE

OBD/CAN
    NONE

TTC
    Radar relative/closing speed

EGO SPEED
    GNSS

AUDIO GATE
    >60 km/h
    hysteresis 62/58 km/h

OPPOSITE VEHICLE
    Display
    No same-lane primary TTC/audio

UNKNOWN
    Display
    No primary audio

DAY/NIGHT
    GNSS sunrise/sunset
    Auto/Day/Night manual

CONFIG
    Touchscreen
    NVS persistent

CONTROL
    No vehicle control

AI/ML
    None in V1
```

---

# 41. Phiên bản tài liệu

```text
Project: Radar Car Distance Warning
Version: V1.0
Platform: ESP32-S3 / PlatformIO / Arduino
UI: LVGL
Status: Architecture defined — implementation pending
```


---

# 42. V1.1 — WEB SERVER / SMARTPHONE / OTA

## 42.1. Mục tiêu

V1.1 bổ sung khả năng để điện thoại/tablet truy cập trực tiếp vào ESP32-S3 qua Wi-Fi nhằm:

- xem Dashboard real-time;
- xem trạng thái radar;
- xem tốc độ GNSS;
- xem 0–5 target;
- xem TTC/risk;
- thay đổi cấu hình;
- lưu cấu hình;
- chạy diagnostic;
- test buzzer;
- cấu hình Wi-Fi;
- chuẩn bị/cung cấp OTA firmware.

Không cần cài app Android/iOS. Người dùng chỉ cần Safari/Chrome.

---

## 42.2. Nguyên tắc kiến trúc

Web UI và LCD UI phải sử dụng **cùng một SystemState và AppConfig**.

Không được tạo một logic xử lý riêng cho Web UI.

```text
                    SystemState
                         │
              ┌──────────┴──────────┐
              ▼                     ▼
          LVGL UI                Web UI
              │                     │
              └──── same state ─────┘
```

Tương tự với configuration:

```text
                 AppConfig
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
      Touchscreen          Web Settings
          │                   │
          └─────────┬─────────┘
                    ▼
              Config Manager
                    │
              Validate + Apply
                    │
                    ▼
              NVS / Preferences
```

---

# 43. Wi-Fi architecture

## 43.1. Mode A — Access Point

ESP32 tạo Wi-Fi riêng.

Ví dụ:

```text
SSID:
RadarCar-XXXX

Password:
********
```

Điện thoại kết nối trực tiếp với ESP32.

Mặc định V1.1:

```text
Wi-Fi mode = AP
```

Ưu điểm:

- không cần Internet;
- không cần router;
- hoạt động độc lập trong xe;
- dễ cấu hình.

---

## 43.2. Mode B — Station

ESP32 kết nối vào một Wi-Fi có sẵn, ví dụ hotspot của điện thoại.

```text
Phone hotspot
       │
      Wi-Fi
       │
      ESP32
```

Settings:

```text
Wi-Fi Mode

○ Access Point
○ Station

SSID: __________

Password: ______

[Connect]
```

Sau khi kết nối phải hiển thị IP:

```text
IP: 192.168.x.x
```

---

# 44. Web Server

Web Server chạy trực tiếp trên ESP32-S3.

URL mặc định ở AP mode:

```text
http://192.168.4.1
```

Có thể hỗ trợ mDNS:

```text
http://radarcar.local
```

nếu môi trường mạng hỗ trợ.

---

# 45. Web UI

Không cần native mobile application.

Công nghệ:

```text
HTML5
CSS
Vanilla JavaScript
WebSocket
REST-like HTTP API
```

Ưu tiên giao diện nhẹ để tiết kiệm:

- Flash;
- RAM;
- PSRAM;
- CPU.

Không đưa framework frontend nặng vào V1.1 nếu không cần thiết.

---

# 46. Responsive design

Web UI phải hoạt động tốt trên:

```text
iPhone / Android
Portrait
Landscape
Tablet
Desktop
```

Ưu tiên mobile-first.

Các nút cấu hình phải đủ lớn để thao tác bằng ngón tay.

---

# 47. Web Dashboard

Trang chính:

```text
┌────────────────────────────────────┐
│ RadarCar                 72 km/h   │
│ GPS ●       Radar ●               │
├────────────────────────────────────┤
│                                    │
│               ROAD                 │
│                                    │
│       ●                            │
│                   ●                │
│                                    │
│                         ●          │
│                                    │
│                                    │
├────────────────────────────────────┤
│ TARGETS: 3                         │
│ TTC: 2.4 s                         │
│ RISK: WARNING                      │
│ AUDIO: ENABLED                     │
├────────────────────────────────────┤
│ Dashboard | Settings | Diagnostics │
└────────────────────────────────────┘
```

Target phải:

- cập nhật real-time;
- di chuyển mượt;
- biểu diễn khoảng cách;
- biểu diễn góc/lateral position;
- đổi màu theo risk;
- biểu diễn OPPOSITE/UNKNOWN khác với SAME_LANE.

---

# 48. WebSocket

WebSocket endpoint:

```text
/ws
```

Dùng để truyền dữ liệu real-time.

Không polling HTTP liên tục cho target tracking.

Ví dụ message:

```json
{
  "type": "state",
  "timestamp": 12345678,
  "egoSpeed": 72.4,
  "targetCount": 3,
  "risk": "WARNING",
  "audioAllowed": true,
  "gnss": {
    "fix": true,
    "satellites": 18,
    "speed": 72.4
  },
  "radar": {
    "online": true,
    "targetCount": 3
  },
  "targets": [
    {
      "id": 1,
      "distance": 32.4,
      "angle": -2.1,
      "relativeSpeed": -8.2,
      "ttc": 3.95,
      "relation": "SAME_LANE",
      "confidence": 0.91
    }
  ]
}
```

---

# 49. REST API

API tối thiểu:

```text
GET  /api/status
GET  /api/targets
GET  /api/config
POST /api/config
POST /api/config/save
POST /api/config/default

GET  /api/radar
POST /api/radar/apply

GET  /api/gnss

GET  /api/diagnostics

GET  /api/wifi
POST /api/wifi

POST /api/buzzer/test
```

Nếu API có thao tác ghi, phải validate dữ liệu trước khi apply.

---

# 50. API status

```text
GET /api/status
```

Trả về:

```json
{
  "uptime": 123456,
  "egoSpeed": 72.4,
  "targetCount": 3,
  "risk": "WARNING",
  "radarOnline": true,
  "gnssFix": true,
  "audioAllowed": true,
  "displayMode": "AUTO"
}
```

---

# 51. API configuration

```text
GET /api/config
```

Trả về toàn bộ AppConfig.

```text
POST /api/config
```

Chỉ thay đổi runtime sau khi:

1. parse;
2. validate;
3. kiểm tra range;
4. apply;
5. trả kết quả.

```text
POST /api/config/save
```

lưu vào NVS.

Có thể gộp Apply + Save nhưng phải có transaction rõ ràng.

---

# 52. Configuration validation

Không cho phép web UI gửi giá trị tùy ý.

Ví dụ:

```text
maxRange:
    10–100 m

audio enable speed:
    30–150 km/h

TTC warning:
    1–10 s

TTC critical:
    0.5–5 s

brightness:
    0–100 %

maxTargets:
    1–5
```

Các giới hạn thực tế cần tập trung trong một module:

```text
ConfigValidator
```

Không rải magic numbers trong code.

---

# 53. Configuration transaction

Khi người dùng thay đổi Settings:

```text
Phone
  ↓
POST /api/config
  ↓
Parse JSON
  ↓
Validate
  ↓
Create temporary config
  ↓
Apply runtime
  ↓
Return success
  ↓
User presses SAVE
  ↓
Write NVS
```

Nếu validation thất bại:

```json
{
  "success": false,
  "error": "ttcCriticalSec must be smaller than ttcWarningSec"
}
```

Không được ghi config lỗi vào NVS.

---

# 54. Touchscreen và Web đồng bộ cấu hình

Nếu người dùng đổi cấu hình trên touchscreen:

```text
Touchscreen
    ↓
Config Manager
    ↓
System Config
    ↓
WebSocket
    ↓
Phone UI
```

Nếu đổi trên điện thoại:

```text
Phone
    ↓
Web API
    ↓
Config Manager
    ↓
System Config
    ↓
WebSocket + LVGL refresh
```

Hai giao diện phải cập nhật lẫn nhau.

---

# 55. Web Settings

Trang Settings:

```text
Settings
├── Radar
├── Safety
├── Lane
├── Audio
├── Display
├── GNSS
├── Wi-Fi
└── Advanced
```

---

# 56. Web Radar Settings

```text
Maximum range          [100] m
Minimum target speed   [5] km/h
Direction              [Both ▼]
Maximum targets        [5]
SNR threshold          [ ... ]
Trigger count          [ ... ]
No-target delay        [ ... ]

[Apply Radar]
[Restore Defaults]
```

---

# 57. Web Safety Settings

```text
Audio enable speed      [60] km/h
Hysteresis              [2] km/h

TTC warning             [3.0] s
TTC critical            [1.5] s

Minimum distance        [3.0] m
Minimum confidence      [0.70]

[Apply]
```

UI phải giải thích:

```text
Audio:
Enable > threshold + hysteresis
Disable < threshold - hysteresis
```

---

# 58. Web Lane Settings

```text
Lane count              [3]
Lane width              [3.4] m
Ego lane                [Center ▼]
Corridor width          [ ... ]

[Calibrate]
```

Calibration phải có màn hình hướng dẫn.

---

# 59. Web Audio Settings

```text
Audio enabled           [ON]

Caution pattern         [Slow]
Warning pattern         [Fast]
Critical pattern        [Continuous]

[Test Buzzer]
```

Nếu dùng active buzzer đơn giản:

```text
Volume
```

không nhất thiết có tác dụng.

---

# 60. Web Display Settings

```text
Display mode

○ Auto
○ Day
○ Night

Day brightness          [100] %
Night brightness        [25] %

Transition               [2.0] s

Show target speed       [ON]
Show TTC                [ON]
Show angle              [OFF]
Show ID                 [OFF]
Animation                [ON]
```

---

# 61. Web GNSS Settings

```text
Fix                    ●
Satellites             18
Current speed          72.4 km/h
Update rate             10 Hz
Speed filter            [Median + EMA]

[GNSS Test]
```

---

# 62. Web Diagnostics

Hiển thị:

```text
System
----------------------
Uptime
CPU usage
RAM
PSRAM
Flash

Display
----------------------
LCD
Touch
FPS
Brightness

Radar
----------------------
Online
Target count
Frame rate
Last frame
Parser errors

GNSS
----------------------
Fix
Satellites
Speed
Last update

Audio
----------------------
Enabled
Gate
Last event

Wi-Fi
----------------------
Mode
SSID
IP
RSSI
Clients
```

---

# 63. Authentication

Web UI phải có ít nhất:

```text
Wi-Fi AP password
+
Web Settings PIN
```

Dashboard có thể mở mà không cần PIN.

Các chức năng thay đổi cấu hình phải yêu cầu authentication.

Ví dụ:

```text
Dashboard
    ↓
Settings
    ↓
Enter PIN
    ↓
Authenticated session
```

Không expose Web Server trực tiếp ra Internet.

Không dùng port forwarding.

---

# 64. Session

Sau khi xác thực:

```text
HTTP session / token
```

được giữ trong thời gian ngắn.

Không lưu PIN dạng plaintext trong frontend JavaScript.

---

# 65. OTA Firmware

Kiến trúc V1.1 phải chuẩn bị OTA.

Menu:

```text
Advanced
└── Firmware
       │
       ├── Current version
       ├── Upload firmware
       └── Reboot
```

Quy trình:

```text
Phone
  ↓
Upload .bin
  ↓
Validate image
  ↓
OTA partition
  ↓
Verify
  ↓
Reboot
  ↓
New firmware
```

Không cho OTA hoạt động bằng một nút vô tình trên Dashboard.

OTA nên yêu cầu:

```text
Advanced
+
PIN
+
Confirmation
```

---

# 66. OTA fail-safe

Nếu firmware mới lỗi:

- không được làm thiết bị mất khả năng boot;
- sử dụng OTA partition phù hợp;
- verify image;
- reboot chỉ sau khi upload thành công.

Nên thiết kế partition scheme có OTA ngay từ đầu.

---

# 67. mDNS

Có thể hỗ trợ:

```text
radarcar.local
```

nhưng đây là tiện ích, không phải cơ chế duy nhất.

Luôn hiển thị IP hiện tại trên Diagnostics.

---

# 68. Wi-Fi status

Dashboard có thể có biểu tượng:

```text
Wi-Fi ●
```

Diagnostics:

```text
Mode: AP
SSID: RadarCar-AB12
IP: 192.168.4.1
Clients: 1
RSSI: -
```

Station:

```text
Mode: STA
SSID: MyPhone
IP: 192.168.43.120
RSSI: -54 dBm
```

---

# 69. Web server resource strategy

Không tải HTML/CSS/JS từ Internet.

Web assets phải nằm trong firmware filesystem, ví dụ:

```text
LittleFS
```

Cấu trúc:

```text
data/
├── index.html
├── style.css
├── app.js
├── dashboard.html
├── settings.html
└── diagnostics.html
```

Build/upload:

```text
PlatformIO
+
LittleFS
```

Ưu tiên gzip/static compression nếu phù hợp với implementation.

---

# 70. Web architecture modules

Thư mục:

```text
src/web/
├── WebServer.cpp
├── WebServer.h
├── WebSocket.cpp
├── WebSocket.h
├── WebAPI.cpp
├── WebAPI.h
├── WiFiManager.cpp
└── WiFiManager.h
```

Có thể thêm:

```text
AuthManager.cpp
OTA.cpp
```

---

# 71. Recommended libraries

Không khóa cứng phiên bản thư viện trước khi build.

Có thể đánh giá:

```text
ESPAsyncWebServer / compatible maintained implementation
AsyncTCP
ArduinoJson
LittleFS
Preferences
WiFi
Update / OTA
```

**Quan trọng:** trước khi chọn dependency, kiểm tra compatibility với ESP32-S3 và phiên bản Arduino-ESP32 hiện tại trong PlatformIO.

Không thêm thư viện nếu functionality có thể thực hiện nhẹ hơn bằng API có sẵn.

---

# 72. WebSocket update rate

Không gửi dữ liệu quá nhanh.

Khuyến nghị:

```text
Dashboard WebSocket: 10–20 Hz
```

Radar có thể xử lý 20–50 Hz nội bộ nhưng Web UI không cần nhận mọi frame.

Có thể gửi:

```text
10 Hz:
    normal dashboard

20 Hz:
    high-speed/diagnostic mode nếu cần
```

---

# 73. Web client behavior

Nếu mất WebSocket:

```text
Web UI:
    show "Disconnected"
```

Sau đó tự reconnect:

```text
1 s
2 s
5 s
...
```

Không reload toàn bộ trang.

Khi reconnect:

```text
GET /api/status
GET /api/config
connect /ws
```

---

# 74. Multiple phones

Có thể cho nhiều client cùng xem Dashboard.

Nhưng cấu hình nên có policy:

```text
Multiple clients:
    Dashboard = allowed
    Settings write = one authenticated session at a time
```

Mục tiêu tránh hai điện thoại đồng thời ghi cấu hình khác nhau.

---

# 75. Safety rule for Web UI

Web UI **không được**:

- điều khiển phanh;
- điều khiển ga;
- điều khiển steering;
- gửi CAN;
- gửi OBD command;
- thay đổi trạng thái điều khiển xe.

Web UI chỉ:

```text
READ
CONFIGURE
DIAGNOSE
UPDATE FIRMWARE
```

---

# 76. Web API không được bypass Safety Engine

Ví dụ không được phép:

```text
Phone
 ↓
API
 ↓
force audio ON
```

hoặc:

```text
Phone
 ↓
API
 ↓
force risk = CRITICAL
```

Web UI chỉ thay đổi configuration hợp lệ.

Risk/TTC vẫn do firmware tính.

---

# 77. Security boundaries

```text
                 Wi-Fi
                   │
                   ▼
              Web Server
                   │
          ┌────────┴─────────┐
          ▼                  ▼
       Read API           Config API
                              │
                         Authentication
                              │
                           Validator
                              │
                        Config Manager
```

Không cho HTTP handler truy cập trực tiếp các biến safety quan trọng.

---

# 78. Updated project structure

```text
radar_car/
│
├── platformio.ini
├── README.md
│
├── src/
│   ├── main.cpp
│   │
│   ├── core/
│   │   ├── SystemState.h
│   │   ├── AppConfig.h
│   │   └── SystemManager.cpp
│   │
│   ├── radar/
│   │   ├── LD2451.cpp
│   │   ├── LD2451.h
│   │   └── protocol.cpp
│   │
│   ├── gnss/
│   │   ├── GNSS.cpp
│   │   └── GNSS.h
│   │
│   ├── tracking/
│   │   ├── TargetTracker.cpp
│   │   └── TargetTracker.h
│   │
│   ├── lane/
│   │   ├── LaneModel.cpp
│   │   └── LaneModel.h
│   │
│   ├── safety/
│   │   ├── TTC.cpp
│   │   ├── RiskEngine.cpp
│   │   └── RiskEngine.h
│   │
│   ├── audio/
│   │   ├── Buzzer.cpp
│   │   └── Buzzer.h
│   │
│   ├── config/
│   │   ├── Config.cpp
│   │   ├── Config.h
│   │   └── ConfigValidator.cpp
│   │
│   ├── ui/
│   │   ├── UI.cpp
│   │   ├── Dashboard.cpp
│   │   ├── Settings.cpp
│   │   └── Theme.cpp
│   │
│   └── web/
│       ├── WebServer.cpp
│       ├── WebServer.h
│       ├── WebSocket.cpp
│       ├── WebSocket.h
│       ├── WebAPI.cpp
│       ├── WebAPI.h
│       ├── WiFiManager.cpp
│       ├── WiFiManager.h
│       ├── AuthManager.cpp
│       └── OTA.cpp
│
└── data/
    ├── index.html
    ├── style.css
    ├── app.js
    ├── dashboard.html
    ├── settings.html
    └── diagnostics.html
```

---

# 79. Updated main loop architecture

```text
setup()
 │
 ├── Serial
 ├── NVS
 ├── LCD
 ├── Touch
 ├── LVGL
 ├── Radar UART
 ├── GNSS UART
 ├── Buzzer
 ├── Wi-Fi
 ├── Web Server
 ├── WebSocket
 └── OTA
 │
 ▼
loop()
 │
 ├── radarTask()
 ├── gnssTask()
 ├── trackerTask()
 ├── laneTask()
 ├── safetyTask()
 ├── audioTask()
 ├── uiTask()
 ├── webTask()
 └── diagnosticsTask()
```

Không sử dụng `delay()` để điều phối các task.

---

# 80. Updated development phases

## Phase 12 — Wi-Fi

1. AP mode.
2. SSID/password.
3. Phone connect.
4. IP display.
5. Station mode.

## Phase 13 — Basic Web Server

1. LittleFS.
2. index.html.
3. CSS.
4. JavaScript.
5. `/api/status`.

## Phase 14 — WebSocket

1. `/ws`.
2. State serialization.
3. Target update.
4. reconnect.
5. multi-client read.

## Phase 15 — Web Dashboard

1. Road view.
2. Target rendering.
3. Ego speed.
4. TTC.
5. Risk.
6. Radar/GNSS status.

## Phase 16 — Web Settings

1. Config API.
2. Validation.
3. Apply.
4. Save.
5. Sync with touchscreen.

## Phase 17 — Authentication

1. Web PIN.
2. Session.
3. Protected configuration API.

## Phase 18 — Diagnostics

1. System.
2. Radar.
3. GNSS.
4. Wi-Fi.
5. memory.
6. performance.

## Phase 19 — OTA

1. Partition.
2. Upload.
3. Validation.
4. Reboot.
5. rollback/fail-safe.

---

# 81. Additional test cases — Web

## W01 — AP

Phone connects to:

```text
RadarCar-XXXX
```

Expected:

```text
192.168.4.1 opens Dashboard
```

## W02 — Dashboard real-time

Move/change radar target.

Expected:

```text
Phone target position updates
without page reload.
```

## W03 — 5 targets

Expected:

```text
TARGETS = 5
```

## W04 — No targets

Expected:

```text
TARGETS = 0
```

## W05 — Settings

Change:

```text
Night brightness = 25%
```

Expected:

- LCD updates;
- web UI updates;
- NVS after Save;
- reboot preserves value.

## W06 — Invalid setting

Send:

```text
TTC critical > TTC warning
```

Expected:

```text
HTTP error
No configuration change
```

## W07 — Two phones

Both can view Dashboard.

Only one authenticated writer can modify settings at a time.

## W08 — WebSocket disconnect

Expected:

```text
Disconnected indicator
Automatic reconnect
```

## W09 — Wi-Fi disconnect

Firmware must continue:

```text
Radar
GNSS
TTC
LCD
Buzzer
```

normally.

Wi-Fi failure must **not** stop the safety processing.

## W10 — OTA success

Upload valid firmware.

Expected:

```text
verify
reboot
new version
```

## W11 — OTA invalid file

Expected:

```text
reject
continue running current firmware
```

---

# 82. Critical reliability rule

**Wi-Fi/Web Server is a secondary interface.**

Nếu Wi-Fi hoặc Web Server bị crash:

```text
Radar        MUST continue
GNSS         MUST continue
Tracker      MUST continue
TTC          MUST continue
Risk         MUST continue
LCD          MUST continue
Buzzer       MUST continue
```

Không được để web request block safety processing.

Web task phải được cô lập khỏi safety-critical processing.

---

# 83. Priority order

Trong firmware:

```text
Priority 1:
Radar receive

Priority 2:
GNSS receive

Priority 3:
Tracking

Priority 4:
TTC/Risk

Priority 5:
Audio

Priority 6:
LCD/UI

Priority 7:
Web/diagnostics
```

Web traffic không được làm trễ radar parser hoặc TTC.

---

# 84. V1.1 Definition of Done

Ngoài toàn bộ V1 checklist, V1.1 phải đạt:

- [ ] ESP32 tạo AP.
- [ ] Điện thoại kết nối được.
- [ ] Dashboard mở bằng browser.
- [ ] Dashboard real-time.
- [ ] WebSocket hoạt động.
- [ ] Hiển thị 0–5 targets.
- [ ] Hiển thị ego speed.
- [ ] Hiển thị TTC.
- [ ] Hiển thị risk.
- [ ] Hiển thị radar/GNSS status.
- [ ] Web Settings hoạt động.
- [ ] Config validation hoạt động.
- [ ] Apply runtime hoạt động.
- [ ] Save NVS hoạt động.
- [ ] Web và touchscreen đồng bộ.
- [ ] Authentication hoạt động.
- [ ] Diagnostics hoạt động.
- [ ] Wi-Fi mất không ảnh hưởng safety engine.
- [ ] Station mode hoạt động.
- [ ] OTA architecture hoạt động.
- [ ] Firmware invalid OTA bị từ chối.
- [ ] Không có API điều khiển xe.

---

# 85. Prompt cập nhật cho Antigravity

Dùng prompt này sau khi mở README.md:

```text
Read README.md completely.

The project specification now includes V1.1 Web Server,
smartphone dashboard, configuration, diagnostics and OTA.

Implement V1.1 incrementally.

Architecture requirements:

1. ESP32-S3 must support Wi-Fi Access Point mode.
2. Also support Station mode.
3. Default mode is Access Point.
4. Smartphone accesses the device using a normal browser.
5. Use responsive mobile-first HTML/CSS/JavaScript.
6. Prefer lightweight Vanilla JavaScript.
7. Store web assets in LittleFS.
8. Provide /api/status.
9. Provide /api/config.
10. Provide configuration validation.
11. Provide WebSocket at /ws.
12. WebSocket must provide real-time system state.
13. Web UI and LVGL must use the same SystemState.
14. Web UI and touchscreen must use the same AppConfig.
15. Configuration changes must go through ConfigManager.
16. Never allow HTTP handlers to directly modify safety variables.
17. Save configuration to NVS only after validation.
18. Add Web Settings pages.
19. Add Diagnostics page.
20. Add Web authentication/PIN for configuration changes.
21. Prepare OTA support and OTA partition layout.
22. Invalid OTA images must not replace the working firmware.
23. Wi-Fi/Web Server failure must never stop radar, GNSS,
    tracker, TTC, risk engine, LCD or buzzer processing.
24. Do not use blocking delay() for runtime scheduling.
25. Do not invent LD2451 protocol fields.
26. Do not invent physical JST pin ordering.
27. Do not add camera, OBD, CAN or vehicle control.
28. Do not add vehicle classification.

Implement in this order:

Phase 12:
Wi-Fi AP + phone connection.

Phase 13:
LittleFS + basic web server.

Phase 14:
WebSocket + SystemState.

Phase 15:
Dashboard.

Phase 16:
Settings + ConfigManager + NVS.

Phase 17:
Authentication.

Phase 18:
Diagnostics.

Phase 19:
OTA.

After each phase:
- compile;
- fix errors;
- run available tests;
- explain changed files;
- do not modify unrelated modules;
- do not proceed if the current phase does not build.

Before implementing a library:
verify compatibility with the current ESP32-S3
Arduino framework / PlatformIO environment.
Avoid unnecessary dependencies.
```

---

# 86. Final V1.1 architecture

```text
                         ┌──────────────────┐
                         │ Smartphone/Tablet │
                         │ Safari / Chrome   │
                         └────────┬─────────┘
                                  │
                              Wi-Fi / WS
                                  │
                                  ▼
┌──────────────────────────────────────────────────────┐
│                    JC3248W535                         │
│                    ESP32-S3                           │
│                                                      │
│  ┌──────────┐     ┌───────────────┐                 │
│  │ LD2451   │────►│ Radar Parser  │                 │
│  └──────────┘     └───────┬───────┘                 │
│                            ▼                         │
│                     Target Tracker                   │
│                            │                         │
│  ┌──────────┐              ▼                         │
│  │ M10N     │──────►   Lane Model                    │
│  └──────────┘              │                         │
│                            ▼                         │
│                      Risk Engine                     │
│                            │                         │
│                    ┌───────┴────────┐                │
│                    ▼                ▼                │
│                  TTC              Audio               │
│                                      │               │
│  ┌───────────────┐                   ▼               │
│  │ SystemState   │──────────────► Buzzer             │
│  └───────┬───────┘                                   │
│          │                                            │
│     ┌────┴──────┐                                     │
│     ▼           ▼                                     │
│  LVGL UI    Web Server                                │
│     │           │                                     │
│  Touchscreen   WebSocket                              │
│                  │                                     │
│             REST API                                   │
│                  │                                     │
│             ConfigManager                              │
│                  │                                     │
│              NVS/OTA                                   │
└──────────────────────────────────────────────────────┘
```

---

# 87. Chốt thiết kế V1.1

```text
MAIN BOARD
    JC3248W535 / ESP32-S3

RADAR
    HLK-LD2451

GNSS
    u-blox M10N 10 Hz

LOCAL UI
    LVGL + 3.5" touchscreen

REMOTE UI
    Web Server + responsive browser UI

NETWORK
    Wi-Fi AP + Station

REAL-TIME
    WebSocket

CONFIG
    REST API + ConfigManager + NVS

AUTH
    Web PIN/session

WEB STORAGE
    LittleFS

UPDATE
    OTA-ready

TARGETS
    0–5

VEHICLE CLASSIFICATION
    NONE

TTC
    Radar relative/closing speed

EGO SPEED
    GNSS

AUDIO
    >60 km/h with hysteresis

OPPOSITE TARGET
    Display only / no same-lane primary audio

DAY/NIGHT
    GNSS sunrise/sunset

CONTROL VEHICLE
    NONE

OBD/CAN
    NONE

CAMERA
    NONE

AI/ML
    NONE

SAFETY PROCESSING
    Independent from Web Server
```
