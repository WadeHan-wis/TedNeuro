#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <stdbool.h>
#include <string.h>
#include "m_i2c.h"
#include "m_ble.h"
#include "m_i2c_rtc.h"
#include "m_i2c_ring_buffer.h"
#include "m_i2c_led.h"
#include "m_i2c_as7341.h"
#include "m_ctrl.h"
#include "nirs_sample.h"
#include "config_app.h"

LOG_MODULE_REGISTER(m_i2c, LOG_LEVEL_INF);

static uint32_t s_seq_num;
static uint16_t s_fixed_gain;
static uint16_t s_fixed_integration_time;

/* Sensor fault recovery(2026-09-28) — 센서별 연속 I2C 실패 횟수. 성공하면 0으로 리셋. */
static uint8_t s_i2c_fault_streak[NIR_SENSOR_COUNT];

/* Ambient light 제거(dark-frame subtraction, 2026-09-28) — lit push 횟수를 세다가
 * DARK_FRAME_LIT_INTERVAL마다 다크 프레임 1개를 추가로 push한다(config_app.h 참고). */
static uint16_t s_dark_frame_tick_counter;

/* NIR1(&i2c1)/NIR2(&i2c0) — pinmap.md §3, 각각 독립 I2C 버스에 실장된 AS7341 2개. */
static m_i2c_as7341_dev_t s_as7341[NIR_SENSOR_COUNT];

/* [버그 수정, 2026-09-29] 부팅 시 센서별 초기화 성공 여부 — 하나라도 실패하면 true인
 * 센서로만 계속 진행한다(둘 다 실패해야 진짜 치명적). 실기로 확인된 문제: 기존에는
 * i2c_init()이 두 센서 중 하나라도 init 실패하면 즉시 포기하고 m_i2c_task_entry()가
 * k_sleep(K_FOREVER)로 잠들어버려, watchdog이 4초마다 SoC를 리셋하는 게 무한 반복됐다
 * (R3/R4 풀업 제거로 NIR1 WHOAMI 실패 재현, RTT 로그로 확인). 이제는 실패한 센서만
 * 배제하고 나머지 센서로 계속 측정 — Acquisition/BLE 독립성·"센서 하나 죽어도 계속
 * 측정"이라는 런타임 fault recovery(recover_as7341_or_escalate())와 동일한 철학을
 * 부팅 시점에도 적용한다. TODO(open-item): 배제된 센서를 런타임 중 주기적으로
 * 재프로브하는 로직은 아직 없음(재부팅해야 복구 시도) — 필요해지면 추가.
 */
static bool s_as7341_ready[NIR_SENSOR_COUNT];

/*
 * LED 상태 시나리오 (요청사항 그대로):
 *   1) 디바이스 On  → LED 3개를 1개씩 1초 간격으로 순차 점등 (3개 LED가 밀착
 *      실장돼 있고 하나는 적외선이라 육안 개별 확인용). 디바이스 Off는 SW1이
 *      하드웨어적으로 전원을 끊어 LED도 자동 소등되므로 별도 코드 경로 없음
 *      (pinmap.md §6 / config_app.h 참고)
 *   2) BLE 연동 성공 → LED 10회 점멸
 *   3) 점멸 종료 후   → 정상 측정 시퀀스(파장별 PWM 스트로빙, source-detector 30mm
 *      기준 광학 설계, architecture.md §5)로 전환
 *
 * AS7341 측정 정확도를 위해 LED는 이 태스크(m_i2c)만 구동한다. BLE 연결 여부는
 * m_ble가 CTRL을 거쳐 전달한다(m_ctrl_is_ble_connected()) — Acquisition/BLE 태스크를
 * 직접 결합하지 않기 위함(architecture.md §2.2).
 */
typedef enum {
	I2C_LED_MODE_DEVICE_ON = 0, /* 디바이스 On, BLE 미연결 — 1개씩 1초 간격 순차 점등 */
	I2C_LED_MODE_BLE_BLINK,      /* BLE 연동 성공 — 10회 점멸 중 */
	I2C_LED_MODE_ACQUISITION,    /* 정상 측정 시퀀스 */
	/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[3]): 장시간 BLE 미연결
	 * 저전력 대기모드. ACQUISITION 중 연결이 끊겨도 BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS
	 * 동안은 계속 측정+버퍼링하다가(로컬 버퍼링 지속), 그 이상 끊겨 있으면 이 모드로
	 * 전환해서 측정을 멈추고(발열/전력 절감) 짧은 점멸로 대기 상태를 표시한다.
	 */
	I2C_LED_MODE_STANDBY,
} i2c_led_mode_t;

static i2c_led_mode_t s_led_mode;
static uint8_t s_blink_toggle_count;
static bool s_blink_led_on;

/* ACQUISITION 중 연속 미연결 tick 카운트 (연결되면 0으로 리셋). STANDBY 점멸용 카운터. */
static uint32_t s_disconnect_tick_count;
static uint32_t s_standby_blink_tick_count;

static uint8_t s_device_on_led_index;
static uint8_t s_device_on_tick_count;

/* BLE AS7341_CONFIG로 설정 가능한 파장별 LED duty (permille). 기본값은 "매뉴얼 조합"
 * 250/500/750‰(=25/50/75%, 640/680/950nm) — 2026-09-28 3박 실측 비교 끝에 고정
 * 운용값으로 확정된 것과 동일(architecture.md §11 항목5 7차 결정). 앱이 CONFIG를
 * 한 번도 안 보내도 확정값 그대로 측정되게 하기 위해 2026-09-29 기존 placeholder
 * (500‰=50/50/50%)에서 변경 — CONFIG write가 들어오면 apply_pending_ble_config()가
 * 그대로 덮어쓴다. */
static uint16_t s_led_duty_permille[NIRS_WAVELENGTH_COUNT] = {250, 500, 750};

/* cycle/active window(offset 6-9) 게이팅 — architecture.md §11 항목4(BLE 실 스택+APK
 * 연동) 완료를 위해 2026-09-18 추가. RTC 100ms tick 주기 자체는 그대로 유지하고
 * (m_ble_proto.h 상단 TODO 참고, §2.3 Bresenham 보정 대상과 분리), ACQUISITION 모드에서
 * "이번 tick에 실제로 측정할지"만 tick 카운터로 게이팅한다. 기본값은 cycle=1/active=1
 * tick(=항상 active, 게이팅 없음) — BLE가 CONFIG를 한 번도 안 보낸 상태(TEMP_AS7341_READ_TEST
 * 등)에서 기존 검증된 매 tick 측정 동작을 그대로 보존하기 위함.
 */
static uint16_t s_gate_cycle_ticks = 1;
static uint16_t s_gate_active_ticks = 1;
static uint32_t s_gate_tick_count;

/* ms를 RTC tick(100ms, SAMPLE_RATE_HZ 고정) 단위로 반올림 변환, 최소 1 tick 클램프
 * (0이면 아래 % 연산이 미정의 동작이 됨). */
static uint16_t ms_to_ticks_clamped(uint16_t ms)
{
	uint32_t tick_ms = 1000U / SAMPLE_RATE_HZ;
	uint32_t ticks = ((uint32_t)ms + tick_ms / 2U) / tick_ms;

	if (ticks < 1U) {
		ticks = 1U;
	}

	return (uint16_t)ticks;
}

/* RTC tick마다(모드 무관) 호출 — BLE로부터 새 AS7341_CONFIG가 도착했으면 적용한다.
 * Acquisition↔BLE 직접 결합 금지 원칙에 따라 m_ctrl을 경유해서만 받는다
 * (m_ctrl.h/m_ble_proto.h 상단 주석 참고, 2026-09-15).
 */
static void apply_pending_ble_config(void)
{
	uint8_t config[CTRL_AS7341_CONFIG_LEN];

	if (!m_ctrl_take_as7341_config(config)) {
		return;
	}

	/* LED1~4 = offset 2~5, %값. 이 보드는 LED가 3개(640/680/950nm)뿐이라 LED4는
	 * 무시한다. */
	for (int wl = 0; wl < NIRS_WAVELENGTH_COUNT; wl++) {
		uint8_t percent = config[2 + wl];

		if (percent > 100) {
			percent = 100;
		}
		s_led_duty_permille[wl] = (uint16_t)percent * 10U;
	}

	/* config[0]은 BLE AS7341_CONFIG의 integration time 필드(u8, 1 unit=20ms)로,
	 * m_i2c_as7341_set_integration_time()의 단위와 동일해서 별도 변환 없이 그대로 넘긴다
	 * (2026-09-21, ATIME 고정+ASTEP 가변 방식으로 레퍼런스와 동일하게 정정). */
	s_fixed_integration_time = config[0];
	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		m_i2c_as7341_set_integration_time(&s_as7341[i], (uint8_t)s_fixed_integration_time);
	}

	/* cycle/active window(offset 6-9) — 위 s_gate_* 주석 참고. active>cycle 클램프는
	 * m_ble_proto.c에서 이미 처리됨. */
	uint16_t cycle_ms = sys_get_le16(&config[6]);
	uint16_t active_ms = sys_get_le16(&config[8]);

	s_gate_cycle_ticks = ms_to_ticks_clamped(cycle_ms);
	s_gate_active_ticks = ms_to_ticks_clamped(active_ms);
	if (s_gate_active_ticks > s_gate_cycle_ticks) {
		s_gate_active_ticks = s_gate_cycle_ticks;
	}
	s_gate_tick_count = 0;

	LOG_INF("BLE config 적용: LED duty(permille)=%u/%u/%u integration=%u(x20ms) gate=%u/%u tick",
		s_led_duty_permille[0], s_led_duty_permille[1], s_led_duty_permille[2],
		s_fixed_integration_time, s_gate_active_ticks, s_gate_cycle_ticks);
}

/* main()에서 호출하지 않는다 — m_i2c_task_entry() 진입 직후 태스크 컨텍스트에서
 * 1회 수행한다 (S2 task_i2c 관례: 드라이버 init은 생성된 task 안에서 진행).
 */
static module_err_t i2c_init(void)
{
	module_err_t err;

	s_seq_num = 0;

	m_i2c_ring_buffer_init();

	err = m_i2c_rtc_init();
	if (err != MODULE_ERR_OK) {
		return err;
	}

	err = m_i2c_led_init();
	if (err != MODULE_ERR_OK) {
		return err;
	}

	/* [버그 수정, 2026-09-29] 두 센서를 서로 독립적으로 시도 — 한쪽이 실패해도 즉시
	 * return하지 않는다(위 s_as7341_ready 주석 참고). 둘 다 실패했을 때만 진짜 치명적. */
	const struct device *as7341_bus[NIR_SENSOR_COUNT] = {
		DEVICE_DT_GET(DT_NODELABEL(i2c1)),
		DEVICE_DT_GET(DT_NODELABEL(i2c0)),
	};
	int ready_count = 0;

	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		module_err_t sensor_err = m_i2c_as7341_init(&s_as7341[i], as7341_bus[i]);

		s_as7341_ready[i] = (sensor_err == MODULE_ERR_OK);
		if (s_as7341_ready[i]) {
			ready_count++;
		} else {
			LOG_ERR("AS7341(%d) 부팅 초기화 실패 — 이 센서는 배제하고 계속 진행 (err=%d)",
				i, sensor_err);
		}
	}

	if (ready_count == 0) {
		/* 두 센서 다 실패 — 측정할 게 아무것도 없으므로 여기서만 기존처럼 전체 포기.
		 * (m_i2c_task_entry()가 error 보고 후 k_sleep(K_FOREVER)로 넘어감) */
		return MODULE_ERR_NOT_INITIALIZED;
	}

	/* [버그 수정, 2026-09-21] I2C bus stuck 재시도(m_i2c_as7341_init() 최대 3회)가 걸리면
	 * 이 지점까지도 시간이 늘어날 수 있어, 아래 블로킹 구간(버전 점멸) 진입 전에 한 번
	 * 더 alive를 보고해둔다 (watchdog stale 오탐 방지, 아래 점멸 루프 주석 참고). */
	m_ctrl_notify_alive(CTRL_ALIVE_I2C);

	/* architecture.md §4/§5: 초기 검증 단계에서는 gain/integration time 고정값 사용.
	 * gain=0(0.5x)/ATIME=0(2.78ms)으로는 실기에서 raw 값이 거의 0(노이즈 수준)이라
	 * 광학 반응 확인이 안 됨을 확인 — gain=9(256x)는 2026-09-15 실기 검증(83ms 적분시간
	 * 기준)에서 6채널 모두 안정적인 비영(非零) 값을 확인한 값이라 그대로 유지한다(레퍼런스
	 * 기본 CFG1 리셋값도 동일하게 9).
	 * 적분시간 기본값은 2026-09-21 "레퍼런스와 동일 세팅" 결정에 따라 _ref_fnirs_example의
	 * AS7341_DEFAULT_INTEGRATION_20MS(=1 unit=20ms 목표, 실제 ATIME=99 고정+ASTEP=70 →
	 * 약 19.7ms)로 정정 — 기존 83ms 대비 짧아졌으므로, 이 값에서도 신호가 노이즈 수준으로
	 * 떨어지지 않는지 다음 실기 검증에서 재확인 필요(TODO open-item, architecture.md §11
	 * 항목5의 gain/ATIME 실측 캘리브레이션과 함께 처리).
	 */
	s_fixed_gain = 9;
	s_fixed_integration_time = 1;
	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		if (!s_as7341_ready[i]) {
			continue;
		}
		m_i2c_as7341_set_gain(&s_as7341[i], s_fixed_gain);
		m_i2c_as7341_set_integration_time(&s_as7341[i], (uint8_t)s_fixed_integration_time);
	}

	/* RTC 시작(ticking)은 여기서 하지 않는다 — BLE bt_enable()이 끝난 뒤
	 * m_i2c_task_entry()에서 시작한다 (아래 sem_ble_init_done 관련 주석 참고).
	 */

	/* 부팅 버전 점멸 표시(config_app.h 상단 주석 참고) — SWD/RTT 없이 육안으로
	 * OTA 적용 여부를 확인하기 위한 정식 기능. K_MSEC 사용은 여기가 RTC tick 루프
	 * 시작 전(부팅 시퀀스 중) 1회뿐이라 문제 없음 — ISR이 아니라 일반 태스크
	 * 컨텍스트(m_i2c_task_entry → i2c_init).
	 */
	/* [버그 수정, 2026-09-21] 이 루프는 FW_VERSION_BOOT_BLINK_COUNT*2*FW_VERSION_BOOT_BLINK_MS
	 * 만큼 블로킹되는데, m_ctrl_notify_alive(CTRL_ALIVE_I2C)는 아래 RTC tick 메인루프에
	 * 들어가야만 호출된다 — watchdog은 CTRL 태스크 시작 즉시 무장되므로(m_ctrl.c
	 * watchdog_init()), 이 루프 시간이 watchdog 타임아웃(4000ms)을 넘어서면 아직 아무
	 * 문제도 없는데 watchdog 리셋이 걸린다(v0.1.13에서 실제 발생 확인, 당시엔 점멸
	 * 횟수가 PATCH에 비례해 계속 늘어나는 구조였음 — 2026-09-22 고정 횟수로 변경돼
	 * 이 위험 자체는 사라졌지만, 매 tick alive 신호는 방어적으로 유지한다). */
	for (int i = 0; i < FW_VERSION_BOOT_BLINK_COUNT; i++) {
		m_i2c_led_all_on(I2C_LED_INDICATOR_DUTY_PERMILLE);
		k_sleep(K_MSEC(FW_VERSION_BOOT_BLINK_MS));
		m_i2c_led_all_off();
		k_sleep(K_MSEC(FW_VERSION_BOOT_BLINK_MS));
		m_ctrl_notify_alive(CTRL_ALIVE_I2C);
	}

	/* 시나리오 1: 디바이스 On → LED 0번부터 1초씩 순차 점등 시작 */
	s_led_mode = I2C_LED_MODE_DEVICE_ON;
	s_device_on_led_index = 0;
	s_device_on_tick_count = 0;

#if TEMP_AS7341_READ_TEST
	/* TEMP(개발용): AS7341 채널 매핑 실측 검증 — BLE 연동 대기 없이 바로 측정 시퀀스로 진입 */
	s_led_mode = I2C_LED_MODE_ACQUISITION;
	m_i2c_led_all_off();
	return MODULE_ERR_OK;
#else
	m_i2c_led_all_off();
	return m_i2c_led_set_duty((nirs_wavelength_t)s_device_on_led_index,
				   I2C_LED_INDICATOR_DUTY_PERMILLE);
#endif
}

/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[4]): module_err.h에 이미
 * 정의돼 있었지만 지금까지 아무도 판정하지 않던 SATURATION/LOW_SIGNAL을 실제로 채운다.
 * I2C 통신 에러가 이미 있으면(호출부에서 status가 OK일 때만 호출) 그쪽을 우선한다.
 * TODO(open-item): 채널별/게인별 정확한 풀스케일 계산은 architecture.md §11의 gain/ATIME
 * 실측 캘리브레이션과 함께 재확정 — 지금은 보수적 고정 임계값(config_app.h) 사용.
 */
static module_err_t check_sensor_sanity(const nirs_sample_t *sample,
					 const bool hw_saturated[NIR_SENSOR_COUNT])
{
	/* 포화 판정(2026-09-28 정정) — 예전엔 raw count가 고정 임계값(AS7341_SATURATION_
	 * THRESHOLD=0xFFFF)을 넘는지로 소프트웨어에서 판정했는데, 이 임계값은 게인/적분
	 * 시간이 바뀌면 실제 풀스케일과 안 맞아 정확하지 않았다(TODO(open-item)였음). 이제는
	 * AS7341 STATUS2의 ASAT_ANALOG/ASAT_DIGITAL 비트(하드웨어가 게인/적분시간과 무관하게
	 * 직접 판정, m_i2c_as7341.c read_raw() 참고)를 그대로 신뢰한다. */
	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		if (hw_saturated[i]) {
			return MODULE_ERR_SENSOR_SATURATION;
		}
	}

	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		for (int wl = 0; wl < NIRS_WAVELENGTH_COUNT; wl++) {
			if (sample->raw[i][wl] < AS7341_LOW_SIGNAL_THRESHOLD) {
				return MODULE_ERR_SENSOR_LOW_SIGNAL;
			}
		}
	}

	return MODULE_ERR_OK;
}

/* 이번 tick이 cycle/active window의 active 구간인지 판정 — 호출부(ACQUISITION 분기)가
 * true일 때만 acquire_one_sample()을 호출한다. */
static bool is_gate_active_tick(void)
{
	return (s_gate_tick_count % s_gate_cycle_ticks) < s_gate_active_ticks;
}

/* AS7341 I2C 통신 실패(트리거/읽기 도중) 시 호출 — 즉시 CTRL에 보고해 안전상태로
 * 래치하지 않고, 먼저 재초기화(WHOAMI+SMUX 재구성, i2c_recover_bus 포함, m_i2c_as7341.c
 * m_i2c_as7341_init() 참고)로 자체 복구를 시도한다. 재초기화가 성공하면 이번 tick 샘플만
 * 유실 처리하고 다음 tick부터 정상 복귀 — 연속 실패가 I2C_FAULT_RECOVERY_MAX_ATTEMPTS회
 * 누적됐을 때만 진짜 하드웨어 고장으로 판단해 CTRL_STATE_DEGRADED로 에스컬레이션한다
 * (단일고장 안전상태 원칙은 유지하되, 복구 가능한 일시적 fault까지 영구 정지시키지는
 * 않는 것이 이번 항목의 목적, config_app.h I2C_FAULT_RECOVERY_MAX_ATTEMPTS 참고).
 */
static void recover_as7341_or_escalate(int sensor_idx, module_err_t err)
{
	module_err_t reinit_err =
		m_i2c_as7341_init(&s_as7341[sensor_idx], s_as7341[sensor_idx].i2c_dev);

	if (reinit_err == MODULE_ERR_OK) {
		/* init()은 gain/integration time을 모르므로(내부적으로 재설정 안 함) 현재
		 * 설정값을 다시 적용해야 다음 tick부터 정상 측정된다. */
		m_i2c_as7341_set_gain(&s_as7341[sensor_idx], s_fixed_gain);
		m_i2c_as7341_set_integration_time(&s_as7341[sensor_idx],
						   (uint8_t)s_fixed_integration_time);
		LOG_WRN("AS7341(%d) I2C fault 복구 성공(재초기화, 직전 streak=%u)", sensor_idx,
			s_i2c_fault_streak[sensor_idx]);
		s_i2c_fault_streak[sensor_idx] = 0;
		return;
	}

	s_i2c_fault_streak[sensor_idx]++;
	LOG_ERR("AS7341(%d) I2C fault 복구 실패 (streak=%u/%u)", sensor_idx,
		s_i2c_fault_streak[sensor_idx], I2C_FAULT_RECOVERY_MAX_ATTEMPTS);

	if (s_i2c_fault_streak[sensor_idx] >= I2C_FAULT_RECOVERY_MAX_ATTEMPTS) {
		m_ctrl_report_error(err);
	}
}

/* dark=true면 LED를 전부 끈 채로(주변광+dark current만) 측정한다 — gain/ATIME/ASTEP은
 * lit과 절대 바꾸지 않는다(같은 조건이어야 앱이 lit_raw-dark_raw로 스케일링 없이 뺄 수
 * 있음, AS7341은 곱연산이 아니라 적분시간에 선형 비례하는 누적연산이기 때문 — 사용자
 * 논의, 2026-09-28). sanity check(포화/저신호)는 다크 프레임에는 적용하지 않는다 —
 * 다크는 원래 low signal이 정상이라 매번 오탐될 것이기 때문.
 */
static void acquire_common(nirs_sample_t *sample, bool dark)
{
	sample->timestamp_us = m_i2c_rtc_get_timestamp_us();
	sample->seq_num = s_seq_num++;
	sample->gain = s_fixed_gain;
	sample->integration_time = s_fixed_integration_time;
	sample->battery_pct = 0; /* TODO(open-item): Battery 모듈 미구현 */
	sample->fw_version = FW_VERSION_PACKED;
	sample->status = MODULE_ERR_OK;
	sample->is_dark = dark;

	for (int wl = 0; wl < NIRS_WAVELENGTH_COUNT; wl++) {
		if (dark) {
			sample->led_duty[wl] = 0;
		} else {
			/* BLE AS7341_CONFIG로 설정된 duty 사용 (기본값 500‰) — apply_pending_ble_config() 참고. */
			sample->led_duty[wl] = s_led_duty_permille[wl];
			m_i2c_led_set_duty((nirs_wavelength_t)wl, sample->led_duty[wl]);
		}
	}

	/* NIR1/NIR2를 먼저 둘 다 트리거해서 적분이 동시에 진행되게 한 뒤, 그 다음에
	 * 순서대로 읽는다 — 트리거→읽기를 센서별로 번갈아 하면 적분 대기시간이 순차로
	 * 더해져 의도한 측정 주기(Cycle)보다 느려짐(v0.1.23, m_i2c_as7341.h 주석 참고). */
	bool triggered_ok[NIR_SENSOR_COUNT] = {false};

	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		if (!s_as7341_ready[i]) {
			/* [버그 수정, 2026-09-29] 부팅 시 초기화 실패로 배제된 센서 — 재시도
			 * 없이(재부팅 전까지 계속 배제, TODO(open-item) 참고) raw는 0으로 둔다. */
			memset(sample->raw[i], 0, sizeof(sample->raw[i]));
			continue;
		}

		module_err_t err = m_i2c_as7341_trigger_measurement(&s_as7341[i]);

		if (err != MODULE_ERR_OK) {
			sample->status = err;
			recover_as7341_or_escalate(i, err);
		} else {
			triggered_ok[i] = true;
		}
	}

	bool hw_saturated[NIR_SENSOR_COUNT] = {false};

	for (int i = 0; i < NIR_SENSOR_COUNT; i++) {
		if (!triggered_ok[i]) {
			/* 이번 tick 트리거부터 실패한 센서는 읽기도 스킵 — 같은 tick에 recovery를
			 * 두 번 시도하지 않는다(위 트리거 루프에서 이미 처리됨). */
			continue;
		}

		module_err_t err = m_i2c_as7341_read_raw(&s_as7341[i], sample->raw[i],
							  &hw_saturated[i]);

		if (err != MODULE_ERR_OK) {
			sample->status = err;
			recover_as7341_or_escalate(i, err);
		} else {
			s_i2c_fault_streak[i] = 0;
		}
	}

	if (!dark && sample->status == MODULE_ERR_OK) {
		module_err_t sanity_err = check_sensor_sanity(sample, hw_saturated);

		if (sanity_err != MODULE_ERR_OK) {
			sample->status = sanity_err;
			LOG_WRN("센서 sanity check 실패: %s (§11 항목6-[4] 실기 검증용 가시화, 2026-09-18)",
				sanity_err == MODULE_ERR_SENSOR_SATURATION ? "SATURATION" : "LOW_SIGNAL");
			m_ctrl_report_error(sanity_err);
		}
	}

	if (!dark) {
		m_i2c_led_all_off();
	}
}

static void acquire_one_sample(nirs_sample_t *sample)
{
	acquire_common(sample, false);
}

/* lit 측정 직후(LED가 이미 꺼진 상태) 호출된다 — 별도로 LED를 끌 필요가 없다. */
static void acquire_one_dark_sample(nirs_sample_t *sample)
{
	acquire_common(sample, true);
}

/* RTC 100ms tick마다 1회 호출. 시나리오 1(LED 1개씩 1초 순차 점등)을 진행하다가,
 * BLE 연동이 확인되면 시나리오 2(점멸)로 전환한다.
 */
static void handle_device_on_tick(void)
{
	if (m_ctrl_is_ble_connected()) {
		s_led_mode = I2C_LED_MODE_BLE_BLINK;
		s_blink_toggle_count = 0;
		s_blink_led_on = false;
		m_i2c_led_all_off();
		return;
	}


	s_device_on_tick_count++;
	if (s_device_on_tick_count < I2C_LED_DEVICE_ON_STEP_TICKS) {
		return;
	}

	s_device_on_tick_count = 0;
	m_i2c_led_set_duty((nirs_wavelength_t)s_device_on_led_index, 0);

	s_device_on_led_index++;
	if (s_device_on_led_index >= NIRS_WAVELENGTH_COUNT) {
		s_device_on_led_index = 0;
	}

	m_i2c_led_set_duty((nirs_wavelength_t)s_device_on_led_index,
			    I2C_LED_INDICATOR_DUTY_PERMILLE);
}

/* BLE 연결이 끊기면 측정을 멈추고 "디바이스 On 순차점등" 상태로 되돌린다
 * (2026-09-16, R2-2 재연결 시나리오 구현 — 연결 해제 후에도 계속 센싱하던 문제 수정).
 * i2c_init()의 시나리오 1 진입 로직과 동일하게 LED 0번부터 다시 순차 점등을 시작한다.
 */
static void reset_to_device_on(void)
{
	m_i2c_led_all_off();
	s_led_mode = I2C_LED_MODE_DEVICE_ON;
	s_device_on_led_index = 0;
	s_device_on_tick_count = 0;
	/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[4]): seq_num은 더 이상
	 * 여기서 리셋하지 않는다 — 부팅 세션 내내 단조증가로 유지해야 앱이 재연결 후
	 * seq_num 불연속을 "로컬 버퍼 오버플로우로 유실된 실측 구간"의 gap 마커로 식별할
	 * 수 있다(m_ble_proto.h 참고). */
	m_i2c_led_set_duty((nirs_wavelength_t)s_device_on_led_index, I2C_LED_INDICATOR_DUTY_PERMILLE);
}

/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[3]): 장시간 BLE 미연결 저전력
 * 대기모드. LED1(640nm)만 짧게 점멸해서 "동작 중이나 연결 대기" 상태를 표시한다. */
static void handle_standby_tick(void)
{
	uint32_t phase = s_standby_blink_tick_count % BLE_STANDBY_BLINK_PERIOD_TICKS;

	if (phase == 0) {
		m_i2c_led_set_duty(NIRS_WAVELENGTH_640NM, I2C_LED_INDICATOR_DUTY_PERMILLE);
	} else if (phase == BLE_STANDBY_BLINK_ON_TICKS) {
		m_i2c_led_set_duty(NIRS_WAVELENGTH_640NM, 0);
	}

	s_standby_blink_tick_count++;
}

/* Safety 인증 대응(2026-09-17): ACQUISITION 중 장시간 미연결로 저전력 대기모드에 진입. */
static void enter_standby(void)
{
	LOG_INF("BLE 연결 끊김 %u tick(%ums) 초과 -> STANDBY 진입 (§11 항목6-[3] 실기 검증용 "
		"가시화, 2026-09-18)",
		s_disconnect_tick_count, s_disconnect_tick_count * (1000U / SAMPLE_RATE_HZ));
	m_i2c_led_all_off();
	s_led_mode = I2C_LED_MODE_STANDBY;
	s_standby_blink_tick_count = 0;
}

static void handle_ble_blink_tick(void)
{
	s_blink_led_on = !s_blink_led_on;

	if (s_blink_led_on) {
		m_i2c_led_all_on(I2C_LED_INDICATOR_DUTY_PERMILLE);
	} else {
		m_i2c_led_all_off();
	}

	s_blink_toggle_count++;

	/* 토글 1회=100ms, 점멸(on+off) 1회=2 toggle → N회 점멸=2N toggle */
	if (s_blink_toggle_count >= (I2C_LED_BLE_CONNECT_BLINK_COUNT * 2)) {
		s_led_mode = I2C_LED_MODE_ACQUISITION;
		s_gate_tick_count = 0; /* cycle/active window를 연결 시점부터 정렬해서 시작 */
		m_i2c_led_all_off(); /* 측정 시퀀스에서 파장별로 개별 점등하므로 우선 소등 */
	}
}

K_SEM_DEFINE(sem_i2c_init_done, 0, 1);

void m_i2c_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	module_err_t init_err = i2c_init();

	/* 성공/실패 무관하게 항상 1회 give — BLE Task가 이 신호를 무한정 기다리며
	 * 멈추지 않게 한다 (m_i2c.h 상단 주석 참고, 2026-09-15 bt_enable() 경합 수정). */
	k_sem_give(&sem_i2c_init_done);

	if (init_err != MODULE_ERR_OK) {
		m_ctrl_report_error(init_err);
		k_sleep(K_FOREVER); /* 획득 하드웨어 없이는 이 태스크가 할 일이 없다 */
	}

	/* BLE bt_enable()이 끝날 때까지 RTC를 시작하지 않는다 — RTC2가 100ms마다 계속
	 * 인터럽트를 발생시키는 상태에서 bt_enable()이 진행되면 SoftDevice Controller
	 * 초기화가 멈추는 문제를 실기에서 확인함(2026-09-15). BLE Task가 bt_enable()
	 * 완료(성공/실패 무관) 후 sem_ble_init_done을 준다.
	 */
	k_sem_take(&sem_ble_init_done, K_FOREVER);

	module_err_t rtc_err = m_i2c_rtc_start();

	if (rtc_err != MODULE_ERR_OK) {
		m_ctrl_report_error(rtc_err);
		k_sleep(K_FOREVER); /* RTC 없이는 샘플링 타이밍을 만들 수 없다 */
	}

	nirs_sample_t sample;

	while (1) {
		k_sem_take(&sem_i2c_ready, K_FOREVER);

		apply_pending_ble_config();

#if TEMP_WATCHDOG_FAULT_INJECT_TEST
		{
			static uint32_t s_fault_inject_tick_count;

			s_fault_inject_tick_count++;
			if (s_fault_inject_tick_count == TEMP_WATCHDOG_FAULT_INJECT_TICKS) {
				LOG_WRN("TEMP_WATCHDOG_FAULT_INJECT_TEST: 지금부터 m_i2c 태스크를 "
					"고의로 멈춥니다 (watchdog 강제 유발 테스트)");
			}
			if (s_fault_inject_tick_count >= TEMP_WATCHDOG_FAULT_INJECT_TICKS) {
				while (1) {
					k_sleep(K_MSEC(100));
				}
			}
		}
#endif

		/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[1]): 단일고장으로
		 * CTRL이 안전상태(DEGRADED/FAULT)에 있으면 LED를 끄고 측정을 완전히 건너뛴다.
		 * 래치 상태라 재부팅 전까지 자동 복구하지 않는다(m_ctrl.h 참고). */
		if (m_ctrl_is_safe_state()) {
			m_i2c_led_all_off();
			m_ctrl_notify_alive(CTRL_ALIVE_I2C);
			continue;
		}

		switch (s_led_mode) {
		case I2C_LED_MODE_DEVICE_ON:
			handle_device_on_tick();
			break;

		case I2C_LED_MODE_BLE_BLINK:
			if (!m_ctrl_is_ble_connected()) {
				reset_to_device_on();
				break;
			}
			handle_ble_blink_tick();
			break;

		case I2C_LED_MODE_ACQUISITION: {
			if (!m_ctrl_is_ble_connected()) {
				/* Safety 인증 대응(2026-09-17, 항목6-[3]): 즉시 멈추지 않고
				 * grace 기간 동안 계속 측정+버퍼링(로컬 버퍼링 지속) —
				 * 아래로 흘러서(fallthrough) 평소처럼 acquire+push한다.
				 * TODO(open-item, architecture.md §11 항목7, 2026-09-18): 테스트 APK가
				 * disconnect 후 자동 재연결을 안 해서 이 grace period를 매번 끝까지
				 * 체감하게 됨 — 펌웨어 동작은 그대로 유지, 앱 쪽에 재연결 로직 추가 필요. */
				s_disconnect_tick_count++;
				if (s_disconnect_tick_count >= BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS) {
					enter_standby();
					break;
				}
			} else {
				s_disconnect_tick_count = 0;
			}

			/* cycle/active window(§11 항목4) — active 구간이 아니면 acquire 자체를
			 * 건너뛴다(LED off 상태 유지, sanity check 오탐 방지, 위 is_gate_active_tick()
			 * 주석 참고). seq_num은 push된 샘플에만 증가하므로 gap 식별(SEQ notify)
			 * 의미는 그대로 유지된다. */
			if (is_gate_active_tick()) {
				/* [버그 수정, 2026-10-01] 아래 dark 보정 sleep 계산에 쓸 "이번 활성
				 * tick이 실제로 언제 시작됐는지" 기록 — 적분시간(nominal)이 아니라
				 * 실제 경과시간을 직접 재기 위함(바로 아래 보정 블록 주석 참고). */
				uint32_t active_tick_start_ms = k_uptime_get_32();

				acquire_one_sample(&sample);

				module_err_t err = m_i2c_ring_buffer_push(&sample);

				if (err == MODULE_ERR_RING_BUFFER_OVERFLOW) {
					LOG_WRN("Ring buffer overflow, dropped_count=%u (§11 항목6-[4] 실기 "
						"검증용 가시화, 2026-09-18)",
						m_i2c_ring_buffer_get_dropped_count());
					m_ctrl_report_error(err);
				}

				/* Ambient light 제거(2026-09-28) — lit DARK_FRAME_LIT_INTERVAL개마다
				 * 다크 프레임 1개를 추가로 push한다. LED는 위 acquire_one_sample()
				 * 끝에서 이미 꺼졌으므로 별도 조치 없이 바로 측정 가능. */
				s_dark_frame_tick_counter++;
				if (s_dark_frame_tick_counter >= DARK_FRAME_LIT_INTERVAL) {
					s_dark_frame_tick_counter = 0;

					nirs_sample_t dark_sample;

					acquire_one_dark_sample(&dark_sample);

					module_err_t dark_err = m_i2c_ring_buffer_push(&dark_sample);

					if (dark_err == MODULE_ERR_RING_BUFFER_OVERFLOW) {
						LOG_WRN("Ring buffer overflow(dark), dropped_count=%u",
							m_i2c_ring_buffer_get_dropped_count());
						m_ctrl_report_error(dark_err);
					}

					/* [버그 수정, 2026-09-30, 2026-10-01 재수정] dark는 lit과 같은 tick
					 * 안에서 추가로 트리거+적분되는 구조라(위 주석 참고), 그 다음
					 * lit이 원래 cycle_ms(예: 300ms) 뒤가 아니라 그보다 늦게
					 * 트리거돼 lit-lit 간격이 늘어남을 실측으로 확인(2026-09-30).
					 * dark 자신의 타임스탬프 정밀도는 보정용이라 중요하지 않고
					 * lit들의 간격이 항상 cycle_ms로 일정한 게 중요하다는 게 확정
					 * 요구사항(사용자 결정) — 여기서 다음 lit을 기존 tick 대기 없이
					 * 곧바로, 정확히 보정된 시간만큼만 재운 뒤 직접 수행해서 lit3
					 * (이번 tick) 시작 시각 기준 정확히 cycle_ms 뒤에 오도록
					 * 강제한다.
					 * [2026-10-01 버그 수정] 최초 구현은 "lit+dark가 각각 정확히
					 * 적분시간(nominal, 예: 100ms)만 걸린다"고 가정하고 보정량을
					 * 계산했는데, 실제로는 트리거+AVALID 폴링+센서 2개 I2C 읽기
					 * 오버헤드가 붙어 lit/dark 각각 적분시간보다 더 걸린다(오실로
					 * 스코프로 active 폭이 공칭 100ms보다 길게 실측됨, 사용자
					 * 2026-10-01 발견) — 그 결과 보정 sleep이 과도해져서 3번째
					 * active마다 cycle_ms보다 지연되는 게 실기로 확인됐다. BLE
					 * 데이터(device_ts)는 0.1초 단위로만 기록돼 이 수십ms 오차가
					 * 안 보였을 뿐, 실제로는 있었다. nominal 적분시간으로 추정하는
					 * 대신 k_uptime_get_32()로 이번 활성 tick이 실제로 시작된
					 * 시각(active_tick_start_ms, 위에서 기록)부터 지금까지 실제
					 * 경과한 시간을 직접 재서, 그 실측값 기준으로만 보정한다. */
					uint32_t tick_ms = 1000U / SAMPLE_RATE_HZ;
					uint32_t cycle_ms = (uint32_t)s_gate_cycle_ticks * tick_ms;
					uint32_t elapsed_ms = k_uptime_get_32() - active_tick_start_ms;

					if (elapsed_ms < cycle_ms) {
						k_sleep(K_MSEC(cycle_ms - elapsed_ms));
					}

					nirs_sample_t next_lit_sample;

					acquire_one_sample(&next_lit_sample);

					module_err_t next_err = m_i2c_ring_buffer_push(&next_lit_sample);

					if (next_err == MODULE_ERR_RING_BUFFER_OVERFLOW) {
						LOG_WRN("Ring buffer overflow, dropped_count=%u",
							m_i2c_ring_buffer_get_dropped_count());
						m_ctrl_report_error(next_err);
					}
					s_dark_frame_tick_counter++;

					/* 이번 tick 안에서 (lit+dark+보정대기+다음 lit)까지 실제로는
					 * cycle_ticks만큼의 시간이 통째로 더 지나갔다 — 아래 공통
					 * trailing s_gate_tick_count++(이 tick 자신의 1회분)와 합쳐
					 * 총 (cycle_ticks+1)회분만큼 카운터를 전진시켜서, 다음
					 * is_gate_active_tick() 판정이 실제 경과 시간과 어긋나지 않게
					 * 한다. 일부러 세마포어를 비우지 않는다 — 그 사이 쌓인 신호
					 * 1개(binary라 최대 1개)는 바로 다음 tick(비활성)이 알아서
					 * 빠르게 소비해 자연스럽게 맞아떨어짐(2026-09-30 시뮬레이션
					 * 검증, 실기 검증 필요). */
					s_gate_tick_count += s_gate_cycle_ticks;
				}
			} else {
				m_i2c_led_all_off();
			}

			s_gate_tick_count++;
			break;
		}

		case I2C_LED_MODE_STANDBY:
			if (m_ctrl_is_ble_connected()) {
				/* 기존 재연결 흐름(10회 점멸) 재사용 후 측정 재개 */
				s_disconnect_tick_count = 0;
				s_led_mode = I2C_LED_MODE_BLE_BLINK;
				s_blink_toggle_count = 0;
				s_blink_led_on = false;
				m_i2c_led_all_off();
				break;
			}
			handle_standby_tick();
			break;
		}

		/* Watchdog(Rev3, R3-1) 생존 신호 — 이 tick의 작업(I2C 읽기 포함)이 끝까지
		 * 진행됐다는 뜻이므로 루프 맨 끝에서 호출한다 (m_ctrl.h 참고). */
		m_ctrl_notify_alive(CTRL_ALIVE_I2C);
	}
}

/* main()에서 생성하지 않는다 — 커널 시작 시 자동 등록/스케줄링된다. */
K_THREAD_DEFINE(m_i2c_tid, I2C_TASK_STACK_SIZE, m_i2c_task_entry, NULL, NULL, NULL,
		 I2C_TASK_PRIORITY, 0, 0);
