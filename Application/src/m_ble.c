#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include "m_ble.h"
#include "m_ble_gatt.h"
#include "m_ble_proto.h"
#include "m_i2c.h"
#include "m_i2c_ring_buffer.h"
#include "m_ctrl.h"
#include "config_app.h"

LOG_MODULE_REGISTER(m_ble, LOG_LEVEL_INF);

/* BLE 지연/재연결 대기 중에도 ring buffer를 계속 소비할 수 있도록 짧은 주기로 polling한다.
 * TODO(open-item): Rev2에서 실제 GATT notify 완료 콜백/큐 기반으로 event-driven 전환.
 */
#define BLE_POLL_INTERVAL_MS 20

/* [버그 수정, 2026-09-21] 재연결 후 ring buffer 백로그(최대 220샘플, config_app.h
 * BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS)를 텀 없이 몰아서 notify하면 Zephyr BLE 스택의
 * ATT 송신 버퍼 풀이 실제 무선 전송 속도보다 빨리 고갈되어 bt_gatt_notify()가 일시적으로
 * -ENOMEM을 반환할 수 있다 — flush_batch()가 이 경우 감지 시(congested) 다음 배치
 * 전송 전에 짧게 대기해서 컨트롤러가 큐를 비울 시간을 준다. */
#define BLE_NOTIFY_CONGESTION_BACKOFF_MS 20

/* [전력 최적화, 2026-09-22] 배치 flush 주기 — "몇 개를 묶을지"는 협상된 MTU가 결정하지만
 * (s_batch_capacity, 프레임 하나에 들어가는 개수), "얼마나 자주 무선을 깨워서 보낼지"는
 * 이 값이 별도로 통제한다. 사용자와 논의 후 결정: 센싱 주기(cycle/active 게이팅에 따라
 * 들쭉날쭉할 수 있음)가 아니라, 전력 절감이 실제로 통제해야 하는 대상인 "무선 송신
 * 빈도" 기준으로 고정 — MTU 협상이 실패해 프레임당 1개씩만 담기는 상황(capacity=1)에도
 * 매 tick(100ms)마다 즉시 전송되는 걸 막아준다(이 경우 이 주기 동안 쌓인 걸 여러 프레임
 * 으로 나눠 한 번에 몰아서 보냄 — 아래 m_ble_task_entry() 참고). 실시간성/배터리
 * 트레이드오프의 초기값 — AT-06 전류소비 실측 후 조정 가능.
 * [2026-09-29] 1000ms → 2000ms로 연장 — dark:lit 비율을 3:1로 낮춘 것과 맞물려 배터리
 * 목표(12h)를 지키기 위한 전력 최적화. 지금 샘플레이트(dark 3:1 기준 약 4.4/s)에서
 * 1000ms 주기면 배치 용량(17개, ATT MTU 협상 후 기준)의 극히 일부(약 4~5개)만 채운 채
 * 매초 무선을 깨우고 있었다 — 2000ms로 늘리면 한 번에 약 9개를 채워서 보내 무선 송신
 * 횟수를 줄인다(실시간성은 그만큼 늦어짐 — 오버나이트 수면측정 용도라 수용 가능하다고
 * 판단). 3000ms(약 13개)까지 더 늘릴 수 있는 여지가 있으나, 우선 2000ms로 보수적으로
 * 시작해서 실측 후 필요하면 더 늘리기로 함(2026-09-29 사용자 결정). 실측 검증 필요.
 */
#define BLE_BATCH_FLUSH_INTERVAL_MS 2000

/* notify 대상 연결 — on_connected/on_disconnected에서만 갱신(BLE Task 컨텍스트 단일 소비). */
static struct bt_conn *s_conn;

/* §6(codingstandard.md): notify 실패를 silent하게 버리지 않기 위한 카운터.
 * 연결 안 됨/notify 미구독 상태의 실패는 정상 상태이므로 별도(s_notify_skip_count)로 센다. */
static uint32_t s_notify_drop_count;
static uint32_t s_notify_skip_count;

/* [전력 최적화, 2026-09-21] 배치 크기 — 연결마다 MTU exchange 콜백에서 실제 협상된
 * ATT MTU 기준으로 다시 계산한다. 협상 전/실패 시 1(=배칭 없음, 그러나 프레임 포맷은
 * 동일하게 header+record 1개)로 안전하게 동작한다. mtu_exchange_cb()(BT RX 스레드)가
 * 쓰고 m_ble_task_entry()(BLE Task)가 읽는 cross-context 값이라 volatile로 충분하다
 * (g_ble_connected와 동일 근거, codingstandard.md §8 — 단순 대입/읽기뿐, 복합 연산 없음). */
static volatile uint16_t s_batch_capacity = 1;

/* [전력 최적화, 2026-09-21] 배치용 임시 버퍼 — ring buffer에서 뽑아 모아뒀다가 한 번에
 * notify한다. **의도적으로 "필요한 만큼만 한 번에 pop"하는 구조**를 쓴다(아래
 * m_ble_task_entry() 참고) — 한 샘플씩 여러 loop 반복에 걸쳐 누적하면, 그 사이 연결이
 * 끊길 경우 이미 ring buffer에서 빠져나온 샘플들이 재연결 안전장치(§11 항목6-[3]
 * ring buffer 버퍼링) 밖에 놓여 유실될 수 있다 — 배치가 다 찼거나 flush timeout이 된
 * 순간에만 pop+즉시 notify를 함께 수행해서 이 위험 구간을 최소화한다. */
static nirs_sample_t s_batch[BLE_PROTO_BATCH_MAX_SAMPLES];
static uint16_t s_batch_count;

/* 테스트 APK(fnirs-visualizer-app-release.apk) 호환용 advertising — AS7341_SERVICE_UUID로
 * 광고해서 APK가 스캔 필터로 찾을 수 있게 한다 (m_ble_gatt.h 상단 주석 참고).
 */
static const struct bt_data s_adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_AS7341_SERVICE_VAL),
};

static const struct bt_data s_scan_rsp_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* bt_enable() 직후(ble_init())와 연결 해제 후(on_disconnected()) 양쪽에서 호출된다.
 * Zephyr peripheral은 연결되면 advertising이 자동 중단되고 연결 해제 후 자동으로
 * 재시작되지 않으므로, 앱이 다시 스캔/연결할 수 있게 매번 명시적으로 시작해야 한다.
 */
static int start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, s_adv_data, ARRAY_SIZE(s_adv_data),
				   s_scan_rsp_data, ARRAY_SIZE(s_scan_rsp_data));

	if (err != 0) {
		LOG_ERR("bt_le_adv_start failed (err=%d)", err);
		return err;
	}

	LOG_INF("BLE advertising started (AS7341_SERVICE_UUID, device name %s)",
		CONFIG_BT_DEVICE_NAME);

	return 0;
}

/* disconnected 콜백 안에서 bt_le_adv_start()를 바로 호출하면, SoftDevice Controller가
 * 연결 해제 HCI 이벤트 처리를 아직 마무리하는 도중이라 광고 시작이 조용히(-EAGAIN 등)
 * 실패할 수 있음을 실기에서 확인(2026-09-16) — LED는 정상적으로 처음 상태로 복귀하는데
 * advertising만 안 돌아오는 증상으로 나타남. 시스템 워크큐로 한 틱 미뤄서 재시작한다.
 */
static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = start_advertising();

	if (err != 0) {
		m_ctrl_report_error(MODULE_ERR_NOT_INITIALIZED);
	}
}

K_WORK_DEFINE(s_adv_restart_work, adv_restart_work_handler);

/* [전력 최적화, 2026-09-21] connection interval을 늘리고 peripheral latency로 유휴
 * connection event를 건너뛰게 하면, 배칭으로 notify 빈도를 줄인 효과가 실제 라디오
 * 절전으로 이어진다(배칭만 하고 이걸 안 하면 라디오는 여전히 자주 깨어나 보낼 데이터가
 * 없는 채로 연결 이벤트만 처리하게 됨). architecture.md §2.4가 원래부터 "S2 선례
 * 범위(7.5~30ms)에서 실측 후 확정"이라고 열어둔 값 — 지금은 그 범위 내 초기값이고,
 * 실측(AT-06 전류소비 검증) 후 조정 필요.
 */
#define BLE_CONN_INTERVAL_UNITS 24  /* 24 × 1.25ms = 30ms */
#define BLE_CONN_LATENCY 4          /* 유휴 시 연결 이벤트 4/5 스킵 → 실효 wake ~150ms */
#define BLE_CONN_TIMEOUT_UNITS 400  /* 400 × 10ms = 4000ms (latency 감안 최소 마진 확보) */

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err != 0) {
		LOG_WRN("ATT MTU exchange 실패(err=%u) — 배치 크기 1로 유지(협상 전 기본값)", err);
		return;
	}

	uint16_t att_mtu = bt_gatt_get_mtu(s_conn);
	uint16_t att_payload = (att_mtu > 3) ? (att_mtu - 3) : 0;
	uint16_t capacity = att_payload / BLE_PROTO_BATCH_RECORD_LEN;

	if (capacity < 1) {
		capacity = 1;
	} else if (capacity > BLE_PROTO_BATCH_MAX_SAMPLES) {
		capacity = BLE_PROTO_BATCH_MAX_SAMPLES;
	}

	s_batch_capacity = capacity;
	LOG_INF("ATT MTU=%u -> 배치 크기=%u샘플/notify (전력 최적화, 2026-09-21)", att_mtu,
		s_batch_capacity);
}

static struct bt_gatt_exchange_params s_mtu_params = {
	.func = mtu_exchange_cb,
};

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		LOG_ERR("BLE connection failed (err=0x%02X)", err);
		return;
	}

	LOG_INF("BLE connected");

	/* DATA0/DATA1 notify 대상 conn을 보관한다 — bt_conn_ref()로 참조를 잡아둬야
	 * 콜백 리턴 이후에도(다음 notify 호출 시점까지) 유효하다. */
	s_conn = bt_conn_ref(conn);

	/* MTU 협상 전까지는 배치 크기를 안전하게 1로 되돌린다 — 이전 연결에서 얻은 값을
	 * 새 연결(다른 중앙기기/앱)에 그대로 쓰면 안 되기 때문. */
	s_batch_capacity = 1;
	s_batch_count = 0;

	int mtu_err = bt_gatt_exchange_mtu(conn, &s_mtu_params);

	if (mtu_err != 0) {
		LOG_WRN("bt_gatt_exchange_mtu 요청 실패(err=%d) — 배치 크기 1로 유지", mtu_err);
	}

	/* [전력 최적화, 2026-09-21] MTU 협상만으로는 실제 무선 PDU 크기가 커지지 않는다 —
	 * LE Data Length Update로 컨트롤러가 협상된 크기(최대 251byte)를 실제로 쓰게
	 * 요청한다. 요청하지 않아도 중앙기기가 먼저 시작할 수 있으나(대부분의 최신 폰),
	 * 테스트 APK가 그렇게 안 할 가능성에 대비해 우리가 먼저 요청한다. */
	if (IS_ENABLED(CONFIG_BT_USER_DATA_LEN_UPDATE)) {
		int dle_err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);

		if (dle_err != 0) {
			LOG_WRN("bt_conn_le_data_len_update 요청 실패(err=%d)", dle_err);
		}
	}

	/* [전력 최적화, 2026-09-21] connection interval/peripheral latency 조정 요청 —
	 * 위 BLE_CONN_* 주석 참고. 중앙기기가 거부/재협상할 수 있다(요청일 뿐 강제 아님). */
	struct bt_le_conn_param conn_param = BT_LE_CONN_PARAM_INIT(
		BLE_CONN_INTERVAL_UNITS, BLE_CONN_INTERVAL_UNITS, BLE_CONN_LATENCY,
		BLE_CONN_TIMEOUT_UNITS);
	int param_err = bt_conn_le_param_update(conn, &conn_param);

	if (param_err != 0) {
		LOG_WRN("bt_conn_le_param_update 요청 실패(err=%d)", param_err);
	}

	/* m_i2c는 이 신호를 polling해서 "디바이스 On 순차점등 -> BLE 10회 점멸 -> 측정"
	 * 전환 시점을 판단한다 (m_ctrl.h 참고, Acquisition/BLE 직접 결합 금지).
	 */
	m_ctrl_notify_ble_connected();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE disconnected (reason=0x%02X)", reason);

	if (s_conn != NULL) {
		bt_conn_unref(s_conn);
		s_conn = NULL;
	}

	/* m_i2c는 이 신호로 "측정 시퀀스 -> 처음 상태(디바이스 On 순차점등)"로 되돌아간다
	 * (m_ctrl.h 참고, 2026-09-16 R2-2 재연결 시나리오 구현). 연결이 끊긴 뒤에도 기기가
	 * 계속 센싱하던 문제 수정.
	 */
	m_ctrl_notify_ble_disconnected();

	/* Zephyr peripheral은 연결되면 advertising이 자동으로 중단되고, 연결 해제 후
	 * 자동으로 재시작되지 않는다 — 앱이 다시 스캔/연결할 수 있도록 재시작해야 한다.
	 * 이 콜백 컨텍스트에서 직접 호출하지 않고 워크큐로 미룬다(위 주석 참고). */
	k_work_submit(&s_adv_restart_work);
}

BT_CONN_CB_DEFINE(m_ble_conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* main()에서 호출하지 않는다 — m_ble_task_entry() 진입 직후 태스크 컨텍스트에서
 * 1회 수행한다 (S2 task_i2c 관례: 드라이버/스택 init은 생성된 task 안에서 진행).
 */
static module_err_t ble_init(void)
{
	m_ble_gatt_init();
	m_ble_proto_init();

	/* AS7341 I2C 초기화가 끝날 때까지 대기 — bt_enable()이 I2C1 SMUX 폴링 도중
	 * 끼어들면 트랜잭션이 멈추는 경합이 확인됨(m_i2c.h 상단 주석 참고, 2026-09-15).
	 * [버그 수정, 2026-09-21] K_FOREVER로 블로킹하면 이 태스크는 m_ctrl_notify_alive
	 * (CTRL_ALIVE_BLE)를 아래 메인루프 진입 전까지 한 번도 호출하지 못한다 — I2C 쪽
	 * 초기화(버전 점멸 등)가 길어지면 watchdog이 두 소스 모두 stale로 보고 리셋시키는
	 * 문제가 있었다(m_i2c.c i2c_init() 버전 점멸 루프 주석 참고). 짧은 타임아웃으로
	 * 나눠 기다리며 매번 alive를 보고해 실제로 멈춘 게 아닌 정상 대기 중임을 알린다. */
	while (k_sem_take(&sem_i2c_init_done, K_MSEC(200)) != 0) {
		m_ctrl_notify_alive(CTRL_ALIVE_BLE);
	}

	int err = bt_enable(NULL);

	if (err != 0) {
		LOG_ERR("bt_enable failed (err=%d)", err);
		return MODULE_ERR_NOT_INITIALIZED;
	}

	if (start_advertising() != 0) {
		return MODULE_ERR_NOT_INITIALIZED;
	}

	/* [전력 최적화, 2026-09-21] MTU 협상/배칭/connection param 조정은 on_connected()에서
	 * 연결마다 수행한다(위 mtu_exchange_cb()/BLE_CONN_* 참고) — S2 정책(§2.4)의 MTU
	 * 251byte 목표를 이제 채택. TX power -8dBm은 prj.conf의
	 * CONFIG_BT_CTLR_TX_PWR_MINUS_8로 적용한다.
	 */
	return MODULE_ERR_OK;
}

/* NIR1(DATA0)/NIR2(DATA1)를 각각 배칭 프레임(m_ble_proto.h v3 스펙)으로 인코드해서
 * notify한다. [전력 최적화, 2026-09-21] 샘플마다 즉시 notify하던 것을 s_batch에 모아
 * 한 번에 보내는 구조로 변경 — 반환값 true = 일시적 혼잡(-ENOMEM) 감지, 호출부가
 * 다음 배치 전송 전에 backoff해야 함.
 */
static bool notify_batch(nir_sensor_id_t sensor_id,
			  int (*notify_fn)(struct bt_conn *, const uint8_t *, uint16_t))
{
	uint8_t frame[BLE_PROTO_MAX_PACKET_LEN];

	uint16_t len = m_ble_proto_encode_batch(sensor_id, s_batch, s_batch_count, frame,
						 sizeof(frame));

	if (len == 0) {
		LOG_ERR("배치 인코드 실패(sensor=%d, count=%u) — 이번 배치 폐기", sensor_id,
			s_batch_count);
		return false;
	}

	int err = notify_fn(s_conn, frame, len);

	if (err == 0) {
		return false;
	}

	if (err == -ENOTCONN || err == -EINVAL) {
		/* 정상 상태(진짜 실패 아님) — §6에 따라 silent하게 버리지 않고 카운터만
		 * 증가시킨다. -ENOTCONN=연결 전/해제 후. -EINVAL=앱이 아직 notify를
		 * 구독(CCC)하지 않은 상태 — Zephyr bt_gatt_notify()가 실제로 이 에러코드를
		 * 쓴다(gatt.c gatt_notify(): !bt_gatt_is_subscribed() 시 -EINVAL 반환,
		 * "통신 실패"가 아니다). 2026-09-17 실기 검증 중 발견: 이전에는 -EINVAL을
		 * 진짜 실패로 오분류해서 MODULE_ERR_BLE_TX_FAILED를 계속 보고했고, 이게
		 * m_ctrl_is_safe_state() 도입 이후로는 앱이 연결만 하고 아직 구독 전인
		 * 정상적인 과도 상태에서 측정을 영구 중단시키는 회귀로 이어질 뻔했다.
		 * 배칭 도입 후에는 실패 시 배치 전체(최대 17샘플)를 한 번에 잃는다는
		 * 점이 단일 샘플 시절과의 차이 — 그래도 여전히 "통신 실패"가 아니라
		 * 카운터만 증가시킨다(§6). */
		s_notify_skip_count++;
		return false;
	}

	if (err == -ENOMEM) {
		/* [버그 수정, 2026-09-21] TX 버퍼 풀 고갈 — architecture.md §11 항목6-[4]가
		 * 이미 "notify는 ACK 없는 fire-and-forget이라 개별 패킷이 무해하게 유실될
		 * 수 있다"고 규정한 것과 같은 성격의 일시적 혼잡이지, 진짜 통신 장애가
		 * 아니다. 재연결 후 ring buffer 백로그(최대 220샘플)를 몰아서 보낼 때
		 * 재현 가능성이 높다. 이걸 MODULE_ERR_BLE_TX_FAILED로 보고하면
		 * CTRL_STATE_DEGRADED로 래치되어(m_ctrl.c) m_i2c 태스크가 측정을 완전히
		 * 멈추고 재부팅 전까지 복구 안 됨 — 정상적인 백로그 flush 상황에서 이
		 * 안전상태 래치가 오발동하는 심각한 회귀였다. -ENOTCONN/-EINVAL과 같은
		 * skip 처리하되, 호출부가 backoff할 수 있게 true를 반환한다. */
		s_notify_skip_count++;
		return true;
	}

	s_notify_drop_count++;
	LOG_WRN("BLE 배치 notify 실패(sensor=%d, err=%d, count=%u), drop_count=%u", sensor_id,
		err, s_batch_count, s_notify_drop_count);
	m_ctrl_report_error(MODULE_ERR_BLE_TX_FAILED);
	return false;
}

/* s_batch에 모인 샘플을 DATA0/DATA1 두 characteristic에 각각 배치 notify로 보내고
 * 카운터를 리셋한다. 호출부(m_ble_task_entry)가 "배치가 다 찼거나 flush timeout이
 * 지났을 때"만 호출한다. */
static bool flush_batch(void)
{
	if (s_batch_count == 0) {
		return false;
	}

	bool congested = false;

	congested |= notify_batch(NIR_SENSOR_1, m_ble_gatt_notify_data0);
	congested |= notify_batch(NIR_SENSOR_2, m_ble_gatt_notify_data1);

	s_batch_count = 0;

	return congested;
}

K_SEM_DEFINE(sem_ble_init_done, 0, 1);

void m_ble_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	module_err_t init_err = ble_init();

	/* 성공/실패 무관하게 항상 1회 give — m_i2c.c가 RTC 시작을 이 신호로 대기한다
	 * (m_ble.h 상단 주석 참고, 2026-09-15 bt_enable()/RTC2 경합 수정). */
	k_sem_give(&sem_ble_init_done);

	if (init_err != MODULE_ERR_OK) {
		m_ctrl_report_error(init_err);
		k_sleep(K_FOREVER); /* BLE stack 없이는 이 태스크가 할 일이 없다 */
	}

	/* [전력 최적화, 2026-09-22] 마지막으로 flush한 시각 — BLE_BATCH_FLUSH_INTERVAL_MS
	 * 간격을 강제하는 기준점. 0이면 "아직 한 번도 flush 안 함"이 아니라 그냥 k_uptime_get()
	 * 기준 과거이므로 최초 데이터 도착 시 바로 flush 조건을 만족한다(문제 없음). */
	int64_t last_flush_uptime = 0;

	while (1) {
		/* Safety 인증 대응(2026-09-17, architecture.md §11 항목6-[3]): 연결이 끊긴
		 * 동안에는 pop하지 않는다 — ring buffer는 이미 overflow 시 가장 오래된
		 * 샘플을 덮어쓰므로(m_i2c_ring_buffer.c), pop을 멈추기만 해도 "짧은 끊김
		 * 동안 로컬 버퍼링 지속"이 자연히 동작한다. m_i2c.c는 끊긴 동안에도 계속
		 * acquire+push한다(reset_to_device_on() 즉시 호출 안 함, m_i2c.c 참고).
		 */
		uint32_t available = m_ctrl_is_ble_connected() ? m_i2c_ring_buffer_count() : 0;
		int64_t now = k_uptime_get();

		if (available == 0 || (now - last_flush_uptime) < BLE_BATCH_FLUSH_INTERVAL_MS) {
			/* 아직 flush 주기가 안 됐거나 보낼 데이터가 없다 — ring buffer에 그대로
			 * 두고(pop하지 않음, 위 주석 참고) 짧게 기다렸다가 다시 확인한다. */
			k_sleep(K_MSEC(BLE_POLL_INTERVAL_MS));
			m_ctrl_notify_alive(CTRL_ALIVE_BLE);
			continue;
		}

		/* [전력 최적화, 2026-09-22] flush 주기가 됐다 — 지금까지 쌓인 걸 전부 비운다.
		 * 한 프레임(s_batch_capacity, MTU가 결정)보다 많이 쌓였으면 여러 프레임으로
		 * 나눠서 이 한 번의 "무선 깨우는 시점"에 몰아 보낸다 — MTU 협상이 잘 안 돼
		 * capacity가 작아도(최악 1), 무선을 깨우는 빈도 자체는 이 루프 한 바퀴로
		 * 통제된다(매 tick마다 즉시 전송되던 문제 방지).
		 */
		uint16_t capacity = s_batch_capacity;

		while (available > 0) {
			uint16_t to_pop = (available < capacity) ? (uint16_t)available : capacity;

			s_batch_count = 0;
			while (s_batch_count < to_pop && m_i2c_ring_buffer_pop(&s_batch[s_batch_count])) {
				s_batch_count++;
			}

			if (s_batch_count == 0) {
				break; /* pop 실패(경쟁 상태로 그 사이 비었음) — 방어적 탈출 */
			}

			bool congested = flush_batch();

			available -= s_batch_count;

			if (congested) {
				/* [버그 수정, 2026-09-21] ATT 송신 버퍼 풀 고갈 시 다음 프레임을
				 * 곧바로 또 시도하면 혼잡이 반복될 수 있다 — 짧게 대기해서
				 * 컨트롤러가 큐를 비울 시간을 준다(위 notify_batch() 주석 참고). */
				k_sleep(K_MSEC(BLE_NOTIFY_CONGESTION_BACKOFF_MS));
			}
		}

		last_flush_uptime = k_uptime_get();

		/* Watchdog(Rev3, R3-1) 생존 신호 (m_ctrl.h 참고). */
		m_ctrl_notify_alive(CTRL_ALIVE_BLE);
	}
}

/* main()에서 생성하지 않는다 — 커널 시작 시 자동 등록/스케줄링된다. */
K_THREAD_DEFINE(m_ble_tid, BLE_TASK_STACK_SIZE, m_ble_task_entry, NULL, NULL, NULL,
		 BLE_TASK_PRIORITY, 0, 0);
