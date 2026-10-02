/*
 * Rev0 | Application-wide configuration constants
 * Task priorities, RTC/timing constants, wavelength definitions.
 * architecture.md §2, §4, §5 참고.
 */
#ifndef CONFIG_APP_H_
#define CONFIG_APP_H_

#ifdef __cplusplus
extern "C" {
#endif

/* --- 보드(PoC v1) 펌웨어 버전 ---
 * v0.0.1부터 시작, 패치 적용마다 PATCH를 1씩 올린다 (v0.0.1 → v0.0.2 → ...).
 * 2026-09-17: OTA(무선 펌웨어 업데이트) 실기 검증 성공을 기점으로 v0.1.1부터
 * MINOR를 1로 올려 새 관리 기준선으로 삼는다(Application/VERSION 파일과 반드시
 * 함께 갱신 — MCUboot 이미지 버전은 이 값이 아니라 그 파일을 따로 읽는다,
 * CHANGELOG.md v0.0.15 참고). 이후 패치는 v0.1.1 → v0.1.2 → ...
 * 버전 이력은 CHANGELOG.md 참고.
 */
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 39

/* nirs_sample_t.fw_version(uint16_t)에 담기 위한 패킹: MAJOR(4bit)|MINOR(4bit)|PATCH(8bit) */
#define FW_VERSION_PACKED \
	(((FW_VERSION_MAJOR & 0xF) << 12) | ((FW_VERSION_MINOR & 0xF) << 8) | \
	 (FW_VERSION_PATCH & 0xFF))

/* --- Task priorities (Zephyr preemptive, lower number = higher priority) ---
 * architecture.md §2.2 / codingstandard.md §4: I2C(Acquisition) > BLE TX > Control
 * 현재 구현 대상은 3개 태스크(m_i2c/m_ble/m_ctrl)로 한정한다.
 * Battery/Temperature, Logging 태스크는 아직 구현하지 않는다.
 */
#define I2C_TASK_PRIORITY    2
#define BLE_TASK_PRIORITY 	 4
#define CTRL_TASK_PRIORITY   6

#define I2C_TASK_STACK_SIZE    2048
#define BLE_TASK_STACK_SIZE    2048	
#define CTRL_TASK_STACK_SIZE   1024

/* --- Sampling / RTC timing (architecture.md §2.3) ---
 * LFCLK 32.768kHz 기준 100ms = 3276.8 tick (비정수).
 * 5-frame 주기로 3277 tick 4회 + 3276 tick 1회 = 16384 tick = 정확히 500ms
 * (2026-09-15 코드 리뷰로 비율이 반대로 구현돼 있던 버그 발견/수정, m_i2c_rtc.c 참고).
 */
#define RTC_LFCLK_HZ                 32768
#define RTC_TICK_TARGET_100MS_LOW    3276
#define RTC_TICK_TARGET_100MS_HIGH   3277
#define RTC_BRESENHAM_FRAME_PERIOD   5   /* HIGH tick이 나오는 주기 (5프레임 중 1회) */

#define SAMPLE_RATE_HZ  10

/* --- Wavelengths (architecture.md §5) --- */
typedef enum {
	NIRS_WAVELENGTH_640NM = 0,
	NIRS_WAVELENGTH_680NM,
	NIRS_WAVELENGTH_950NM,
	NIRS_WAVELENGTH_COUNT,
} nirs_wavelength_t;

/* --- NIR 센서 (AS7341 x2, pinmap.md §3: 각각 독립 I2C 버스) --- */
typedef enum {
	NIR_SENSOR_1 = 0, /* &i2c1, SCL=P0.09/SDA=P0.10 */
	NIR_SENSOR_2,      /* &i2c0, SCL=P0.08/SDA=P0.07 */
	NIR_SENSOR_COUNT,
} nir_sensor_id_t;

/* --- Ring buffer (architecture.md §2.5: 100~200 샘플 원안) ---
 * [버그 수정, 2026-09-21] 원안(100~200)은 BLE notify 지연/혼잡(5~10초) 버티기만 고려한
 * 값이었는데, 이후 §11 항목6-[3] Safety 대응으로 도입된 BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS
 * (연결 끊겨도 30초는 계속 측정+버퍼링)와 서로 검토 없이 따로 정해져 있었다 — 150샘플(15초
 * 분량)로는 30초 grace period의 절반도 못 버티고 앞부분 데이터가 실시간으로 덮어써져
 * 유실됐다(m_i2c_ring_buffer_push() overflow 시 가장 오래된 샘플 덮어쓰기, silent는
 * 아니지만 dropped_count만 늘고 실제로는 유실). BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS를
 * 그대로 참조해 두 값이 다시 어긋나지 않게 한다(정의는 config_app.h 아래쪽, 매크로라 순서
 * 무관 — 실제 확장은 이 파일 전체가 include된 뒤 일어난다).
 */
#define RING_BUFFER_CAPACITY BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS

/* --- LED 상태 시나리오 (m_i2c.c가 관리, pinmap.md §6) ---
 * SW1은 MAX16054를 통해 VBAT를 하드웨어적으로 on/off한다 — MCU에는 GPIO로 연결되어
 * 있지 않다. 즉 "디바이스 On"은 곧 "부팅됨"이고, "디바이스 Off"는 전원이 물리적으로
 * 끊긴 것이라 소프트웨어로 표현할 상태가 아니다 (LED도 전원과 함께 자동 소등).
 *
 * 시나리오: 부팅(디바이스 On) → LED 3개를 1개씩 1초 간격 순차 점등(3개 LED가 밀착
 * 실장돼 있고 하나는 적외선이라 육안 개별 확인용, 2026-09-14 실기 테스트 요청) →
 * BLE 연동 성공 시 10회 점멸 → 측정 시퀀스(파장별 PWM 스트로빙,
 * m_i2c_task_entry의 acquire_one_sample) 시작.
 */
#define I2C_LED_INDICATOR_DUTY_PERMILLE 200 /* 20% duty — 개발 단계 눈부심 방지 (2026-09-15 변경, 기존 50) */

/* 디바이스 On 상태에서 LED 1개당 점등 유지 시간. RTC 100ms tick을 그대로 세므로
 * SAMPLE_RATE_HZ(10)개 tick = 1초.
 */
#define I2C_LED_DEVICE_ON_STEP_TICKS SAMPLE_RATE_HZ

/* TEMP(하드웨어 진단용, 2026-09-14): D3/D4 미점등 원인 파악을 위해 1이면 순차 점등 대신
 * 3개 LED를 100% duty로 계속 켜둔다(타이밍 걱정 없이 멀티미터로 측정 가능).
 * 진단(R17/R20 2.7V 동일, D4 육안 확인) 완료 — 순차 점등 모드로 복귀.
 * [정리, 2026-09-28] 진단 완료 후 오래 방치돼있던 훅이라 플래그/사용처를 제거했다
 * (memory_ram_optimization_plan.md 항목3).
 */

/* BLE 연동 성공 점멸 횟수. 점멸 토글은 RTC 100ms tick을 그대로 사용한다
 * (토글 1회=100ms, 점멸 1회=on+off=2 tick=200ms → 10회 점멸 = 2초).
 */
#define I2C_LED_BLE_CONNECT_BLINK_COUNT 10

/* 부팅 버전 점멸 표시 (2026-09-17, architecture.md §11 항목6-[6] OTA 검증 과정에서
 * 도입 — TEMP_OTA_TEST_MARKER를 대체하는 정식 기능). SWD/RTT 연결 없이(조립된 상태)
 * 육안으로 "부팅이 이 지점까지 정상 진행됐는지"를 확인하기 위한 기능이다.
 * 부팅 직후(디바이스 On 순차점등 진입 전) LED 3개를 동시에 FW_VERSION_BOOT_BLINK_COUNT
 * 회 점멸한다.
 * [버그 수정, 2026-09-22] 원래는 (FW_VERSION_PATCH+1)회로 패치 버전에 따라 점멸
 * 횟수가 계속 늘어나는 구조였다 — "업데이트 전/후 점멸 횟수 차이로 OTA 성공 여부
 * 확인"이 목적이었으나, PATCH가 매 패치 +1되는 정책상 버전이 쌓일수록 이 블로킹
 * 구간 자체가 계속 길어져 watchdog 타이밍과 충돌하는 버그로 실제 이어졌다(v0.1.13→
 * v0.1.14, CHANGELOG 참고). 사용자 결정으로 고정 3회로 변경 — OTA 적용 여부 확인은
 * 이제 BLE `AS7341_VERSION` characteristic(0x1528)으로 fw_version을 읽는 방식을
 * 사용한다(앱이 이미 지원). 이 점멸은 "부팅이 여기까지 왔다"는 생존 신호 용도로만
 * 남는다.
 */
#define FW_VERSION_BOOT_BLINK_MS 150
#define FW_VERSION_BOOT_BLINK_COUNT 3

/* TEMP(개발용, 2026-09-15): AS7341 채널-파장 매핑 실측 검증용. 1이면 BLE 연동 대기 없이
 * 부팅 즉시 측정 시퀀스(acquisition)로 진입해서 raw 채널 값을 RTT로 바로 확인할 수 있다.
 * 실제 BLE(Rev2)가 준비되기 전까지의 임시 우회이며, 검증 끝나면 0으로 되돌린다.
 */
#define TEMP_AS7341_READ_TEST 0

/* [정리, 2026-09-28] RTC tick 주기 실측(TEMP_RTC_TICK_LOG_TEST)과 BLE 스택 격리 진단
 * (TEMP_BLE_DISABLE_TEST) 훅은 둘 다 2026-09-15 당시 문제를 검증/해결한 뒤 0으로
 * 방치돼있던 초기 개발용 임시 코드라 플래그/사용처를 제거했다
 * (memory_ram_optimization_plan.md 항목3).
 */

/* TEMP(개발용, 2026-09-16): 테스트 APK로 수신되는 DATA0/DATA1 raw 값이 AS7341이 실제로
 * 읽은 값과 일치하는지 검증하기 위해, RTT 로그(m_i2c_as7341.c의 F5~NIR LOG_DBG)를
 * 앱 연동 중에도 확인할 수 있게 임시로 로그 레벨을 올린다. 검증 끝나면 0으로 되돌린다.
 */
#define TEMP_AS7341_RAW_DBG_LOG 1

/* Watchdog (Rev3, R3-1, 2026-09-16): m_i2c/m_ble Task가 각자 루프마다 m_ctrl_notify_alive()로
 * 생존 신호를 보내고, m_ctrl은 둘 다 최근 WATCHDOG_ALIVE_STALE_MS 이내에 응답했을 때만
 * 하드웨어 watchdog을 feed한다 — I2C 버스 hang 등으로 한쪽이라도 멈추면 feed가 끊겨서
 * SoC가 자동 리셋된다. 온도/배터리 센서가 없는 PoC v1 보드(architecture.md §11)에서
 * 구현 가능한 유일한 Rev3 안전장치라 우선 적용한다.
 * TIMEOUT은 AS7341 STATUS2 폴링 최악 케이스(NIR1+NIR2 순차 최대 400ms, CHANGELOG 참고)와
 * RTC 100ms tick 주기에 충분한 여유를 둔 값 — 실측 후 필요하면 조정한다.
 */
#define WATCHDOG_TIMEOUT_MS 4000
#define WATCHDOG_ALIVE_STALE_MS 1000
#define WATCHDOG_CHECK_PERIOD_MS 500

/* --- Sensor(AS7341) fault recovery (Rev3 R3-1, 2026-09-28) ---
 * I2C 트리거/읽기 실패(MODULE_ERR_I2C_TIMEOUT) 시 하드웨어 watchdog(위)이 SoC 전체를
 * 리셋할 때까지 기다리지 않고, 먼저 AS7341 재초기화(WHOAMI+SMUX 재구성, i2c_recover_bus
 * 포함 — m_i2c_as7341_init() 참고)로 자체 복구를 시도한다. 연속 실패가 이 횟수에
 * 도달해야만 진짜 하드웨어 고장으로 판단해 `m_ctrl_report_error()`로 에스컬레이션한다
 * (그 뒤 처리는 아래 CTRL_MODULE_FAULT_REBOOT_MAX 참고) — 일시적 I2C 글리치까지 곧장
 * 정지시키지 않기 위함(m_i2c.c recover_as7341_or_escalate() 참고).
 */
#define I2C_FAULT_RECOVERY_MAX_ATTEMPTS 3

/* --- 모듈 오류 재부팅 에스컬레이션 (2026-09-29, TedreamS2 재시도 로직 참고 적용) ---
 * `m_ctrl_report_error()`로 보고된 오류가 기존에는(v0.1.31까지) 확인 즉시
 * CTRL_STATE_DEGRADED로 영구 래치(재부팅해야만 해제)됐는데, 사용자 결정에 따라
 * TedreamS2 재시도 로직을 우리 구조에 맞게 변형해 적용한다: 이 횟수만큼은 먼저
 * 소프트웨어 재부팅(`m_ctrl_request_reset()`)으로 복구를 시도해보고, 그래도 같은
 * 계열의 오류가 재발하면 그때 비로소 영구 안전상태로 전환한다(`m_ctrl.c
 * escalate_or_reboot()` 참고). 재부팅 횟수는 GPREGRET(POR 전에는 지워지지 않는
 * 레지스터)에 저장 — 즉 "물리적으로 전원을 껐다 켜야만" 카운트가 초기화된다
 * (TedreamS2의 "재부팅 3회도 실패하면 전원 off" 4단계를, 우리 보드는 SW로 전원을
 * 끌 수 없어(물리 SW1뿐) "더 이상 재부팅하지 않는 영구 정지"로 대체한 것).
 * **주의**: 이 카운트는 부팅 세션 내내 누적되며 성공적으로 복구돼도 리셋되지
 * 않는다(TedreamS2 원 로직 그대로) — 서로 무관한 오류 3번이 쌓여도 마지막엔 영구
 * 정지된다. 의료기기 단일고장 안전 철학(IEC 60601)과의 정합성은 아직 정식
 * 검토되지 않은 상태 — TODO(open-item, architecture.md §11 규제 항목과 연결).
 */
#define CTRL_MODULE_FAULT_REBOOT_MAX 3

/* TEMP(watchdog 실기 fault injection 테스트용, 2026-09-17): 1이면 m_i2c 태스크가
 * 부팅 후 TEMP_WATCHDOG_FAULT_INJECT_TICKS번째 tick에서 완전히 멈춘다(m_ctrl_notify_alive
 * 호출 중단) — feed_watchdog_if_alive()가 stale을 감지해 feed를 멈추고, WATCHDOG_TIMEOUT_MS
 * 후 하드웨어가 SoC를 강제 리셋하는지, 재부팅 후 log_reset_cause()가 이 리셋을
 * watchdog으로 정확히 로그하는지 확인하기 위함. 검증 끝나면 반드시 0으로 되돌린다.
 */
#define TEMP_WATCHDOG_FAULT_INJECT_TEST 0
#define TEMP_WATCHDOG_FAULT_INJECT_TICKS 20

/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[3]): BLE 연결이 끊겨도
 * 즉시 측정을 멈추지 않고 ring buffer에 계속 버퍼링하다가, 이 tick 수를 넘겨도 재연결이
 * 안 되면 저전력 대기모드(I2C_LED_MODE_STANDBY)로 전환한다. RTC tick = 100ms(SAMPLE_RATE_HZ)
 * 기준. 제품 정책값 아님 — 실측 후 조정 가능한 초기값이다.
 * [버그 수정, 2026-09-21] 원래 300 tick(30초)이었으나 RING_BUFFER_CAPACITY(위 §2.5)가
 * 이 값을 그대로 참조하도록 묶으면서, RAM 예산(nRF52832 64KB) 안에서 안전 마진을 두고
 * 감당 가능한 220 tick(22초)으로 낮췄다 — 300으로 두면 Application 이미지 RAM 사용률이
 * 96.07%(여유 2.5KB)까지 올라가 스택 오버플로우 등 위험이 있음을 실측 확인(2026-09-21,
 * 사용자 결정: grace period를 버퍼가 감당 가능한 수준으로 단축). 220으로는 RAM 사용률
 * 약 90%대 유지, 22초 내내 데이터 유실 없이 보장된다(그 이상 끊기면 STANDBY 진입 후 측정
 * 자체가 중단되므로 추가 유실 없음, 아래 STANDBY 전환 로직 참고).
 */
#define BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS 220

/* STANDBY 중 "살아있음 + 연결 대기 중"을 알리는 저전력 점멸 패턴 — LED1(640nm)만
 * BLE_STANDBY_BLINK_PERIOD_TICKS(2초)마다 BLE_STANDBY_BLINK_ON_TICKS(100ms) 짧게 켠다.
 */
#define BLE_STANDBY_BLINK_PERIOD_TICKS 20
#define BLE_STANDBY_BLINK_ON_TICKS 1

/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[4]): AS7341 채널 raw 값
 * sanity check 임계값. LOW_SIGNAL은 노이즈 플로어 근접값으로 판정한다.
 * [정정, 2026-09-28] SATURATION은 더 이상 이 고정 임계값으로 판정하지 않는다 —
 * AS7341 STATUS2의 ASAT_ANALOG/ASAT_DIGITAL 비트(하드웨어가 게인/적분시간과 무관하게
 * 직접 판정, m_i2c_as7341.c read_raw()/m_i2c.c check_sensor_sanity() 참고)로 대체돼
 * "채널별/게인별 정확한 풀스케일 계산" TODO 자체가 해소됐다.
 */
#define AS7341_LOW_SIGNAL_THRESHOLD 10

/* --- Ambient light 제거(dark-frame subtraction, 2026-09-28, 비율 2026-09-29 조정) ---
 * lit(정상 측정) 샘플 DARK_FRAME_LIT_INTERVAL개마다 다크 프레임(LED 전부 OFF, 같은
 * gain/ATIME/ASTEP)을 1개 추가로 삽입한다. 다크(주변광+dark current)는 생리신호(맥동)보다
 * 훨씬 느리게 변하므로 lit과 1:1로 잴 필요가 없다는 원칙은 그대로 유지.
 * [2026-09-29] 원래 10:1이었는데, dark 기준점의 최신성(정밀도)을 조금 더 높이기 위해
 * 3:1로 변경 — 절충안(사용자 결정). 주의: dark 프레임은 별도 tick이 아니라 해당 활성
 * tick 안에서 lit 측정 직후 추가로 한 번 더 트리거+적분하는 방식(m_i2c.c acquire_common()
 * 호출부 참고)이라, 이 값을 낮출수록 AS7341/I2C 활성 듀티가 유의미하게 늘어난다(10:1일 때
 * 평균 활성 듀티 약 36% → 3:1일 때 약 43%, 1:1이면 약 66%까지 증가 — 실측 없이 추정한
 * 구조적 계산이며 실측 검증 필요). BLE 전송량도 그만큼 늘어나므로 §6 전력 최적화
 * 항목(TX power/batch flush interval)과 함께 검토해서 정함 — 2026-09-29 대화 참고.
 * lit 샘플레이트/ring buffer 용량(RING_BUFFER_CAPACITY, 위 §2.5)에는 영향 없음 — 정적
 * 배열 크기가 아니라 실제 push 빈도만 늘어나는 것이라 기존 용량 여유로 흡수된다.
 */
#define DARK_FRAME_LIT_INTERVAL 3

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_APP_H_ */
