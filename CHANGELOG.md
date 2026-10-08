# fNIRS 펌웨어 변경 이력 (PoC v1 보드)

`Application/include/config_app.h`의 `FW_VERSION_MAJOR/MINOR/PATCH`와 1:1로 대응한다.
패치 적용마다 PATCH를 1씩 올린다 (v0.0.1 → v0.0.2 → ...).

## v0.0.1 (2026-09-14)

- Rev0 Zephyr(NCS) 스캐폴드: `m_i2c`/`m_ble`/`m_ctrl` 3개 태스크, RTC 100ms ISR(5-frame
  Bresenham 보정), CTRL 상태머신.
- PoC v1(nRF52832) 실기 부팅 크래시 2건 수정:
  - `NRFX_RTC_INSTANCE(2)` → `NRFX_RTC_INSTANCE(NRF_RTC2)` (포인터 캐스팅 버그, MemManage 폴트)
  - RTC2 IRQ(36) `IRQ_CONNECT` 누락 → 추가 (Spurious IRQ 폴트)
- NFC 핀(P0.09/P0.10) → GPIO 전환 (`app.overlay`, NIR1 I2C 사용을 위해 필요)
- 로깅 백엔드를 RTT로 전환 (PoC v1은 UART 핀 미노출)
- LED PWM 실제 구동 구현 (PWM0 채널 0/1/2, P0.04/05/06) — devicetree 기본 `nordic,invert`
  제거(우리 회로는 active-high)
- LED 상태 시나리오: 디바이스 On 시 LED 1개씩 1초 간격 순차 점등 → BLE 연동 성공 시
  10회 점멸 → 측정 시퀀스 전환
- **알려진 이슈**: D3(680nm)/D4(950nm 스펙, PoC v1 실장 980nm) LED가 점등되지 않음 —
  D2(640nm)만 정상 동작 확인. 소프트웨어(devicetree/PWM 채널 매핑)는 D2와 구조적으로
  동일하게 확인됨 — 하드웨어(Q5/Q6, R18/R20, 배선) 원인 가능성 조사 중.

## v0.0.2 (2026-09-15)

- D3/D4 LED 미점등 1차 진단: 채널 배열 스왑 + `led_test` 브랜치 완전 격리 테스트(태스크/RTC/I2C
  전부 배제, PWM만) + GPIO 스윕 테스트까지 전부 고장 재현 → **하드웨어 결함으로 오판**
  (아래 v0.0.3에서 정정됨).
- 핀맵 재확인: `IR_LED_CTRL1=P0.04, WH_LED_CTRL2=P0.05, IR_LED_CTRL2=P0.06` 매핑을
  스키매틱 재열람 + 레퍼런스 펌웨어(`WisMedical/examples`) 핀 정의로 3중 확인·확정.

## v0.0.3 (2026-09-15)

- **D3/D4 미점등 진짜 원인 확정 (하드웨어 결함 아니었음, v0.0.2 결론 정정)**: 빌드용 스탠드인
  보드 `nrf52dk/nrf52832`의 기본 devicetree가 UART0을 활성화하고 있었고, 그 기본 핀이
  `UART_TX=P0.06(D4), UART_RTS=P0.05(D3)`라 PWM0과 물리 핀을 경합하고 있었음.
  `CONFIG_LOG_BACKEND_UART=n`은 로깅 백엔드만 끌 뿐 UART0 페리페럴/pinctrl 자체는 계속
  활성 상태였던 게 원인. `app.overlay`에 `&uart0 { status = "disabled"; };` 추가로 해결
  (`pinmap.md` §8 참고). D2가 항상 정상이었던 이유(UART가 안 쓰는 핀)까지 정합적으로 설명됨.
- `led_test` 브랜치(격리 PWM 테스트, GPIO 스윕 테스트) 진단 종료, `main`으로 병합 없이 복귀.
- LED 인디케이터 duty 50%→20% (개발 단계 눈부심 방지).

## v0.0.4 (2026-09-15)

- AS7341(NIR1) I2C 드라이버 포팅 (`m_i2c_as7341.c`, `app.overlay`의 `&i2c1` 노드,
  SCL=P0.09/SDA=P0.10, I2C1/TWIM1 사용).
- 실기 검증: WHOAMI 레지스터(0x92) 읽기 성공, 상위 6비트가 예상값 `0x24`와 일치 —
  NIR1 I2C 통신 자체가 정상 동작함을 확인.
- **버그 수정**: SMUX RAM(20바이트) 구성 burst write가 레지스터주소(1)+데이터(20)=21바이트로,
  `i2c_nrfx_twim` 드라이버 기본 내부 버퍼(16바이트)를 초과해 `-ENOSPC(-28)`로 실패
  (실기 RTT 로그로 확인). `app.overlay`의 `&i2c1`에 `zephyr,concat-buf-size = <32>;` 추가로 해결,
  재검증 결과 "SMUX 구성 성공" 로그 확인.
- NIR1 `m_i2c_as7341_init()` 전체 초기화 시퀀스(WHOAMI 확인 → PON → SMUX 구성) 실기 완료.
- TODO(open-item, 미해결): SMUX RAM 20바이트 설정값과 채널→파장(640/680/950nm) 매핑은
  통상적 레퍼런스 값 기반 임시 설정 — ams AS7341 데이터시트로 재검증 필요 (파장별 분리
  정확도 검증 단계에서 확정). NIR2(SCL=P0.08, SDA=P0.07, I2C0)는 아직 미착수.

## v0.0.5 (2026-09-15)

- **AS7341 datasheet(DS000504 v3-00) 기준 SMUX 매핑 오류 발견 및 수정**: v0.0.4의 SMUX 구성이
  F1~F4(415/445/480/515nm)+Clear+NIR 그룹으로 되어 있었으나, 우리 프로젝트 파장
  640/680/950nm에 실제로 가장 가까운 필터는 F7(630nm)/F8(680nm)/NIR(910nm)이다
  (datasheet Figure 7/22). SMUX를 F5~F8+Clear+NIR 그룹으로 교체.
  TODO(open-item): 정확한 SMUX RAM 바이트 값은 이 datasheet에 없음(ams AN000633 앱노트
  별도 필요) — 공개 레퍼런스 구현(Adafruit AS7341 드라이버) 값을 우선 적용, 추후
  공식 앱노트로 바이트 단위 교차검증 필요.
- 채널→파장 매핑 수정: raw[640nm]=CH2(F7), raw[680nm]=CH3(F8, 정확 일치), raw[950nm]=CH5(NIR).
- 드라이버 레지스터/비트 정의(AS7341_REG_*, ENABLE 비트, SMUX 채널 인덱스 등)를
  `m_i2c_as7341.c`에서 `m_i2c_as7341.h`로 이동 (설정값은 헤더에 정의).
- 실기 data read 검증: gain=0(0.5x)/ATIME=0(2.78ms) 고정값으로는 raw 값이 거의 0(노이즈
  수준)이라 광학 반응 확인이 안 됨을 확인 → gain=9(256x)/ATIME=29(약 83ms)로 변경 후
  6채널(F5/F6/F7/F8/Clear/NIR) 모두 안정적인 비영 값 확인 (RTT 로그: F5=4960 F6=6744
  F7=5015 F8=3280 Clear=19275 NIR=1593 — Clear가 가장 높고 NIR이 낮은 것은 실내 조명
  환경에서 물리적으로 타당). SMUX/게인 경로가 실제로 동작함을 실측 확인.
- TODO(open-item): 알려진 파장(640/680nm) LED를 센서에 직접 비추는 정량 검증은 아직
  미실시 — 채널 상대값의 정성적 타당성만 확인된 상태.

## v0.0.6 (2026-09-15)

- **NIR2 AS7341 포팅 착수** (`&i2c0`, SCL=P0.08/SDA=P0.07, pinmap.md §3). `m_i2c_as7341` 드라이버를
  인스턴스 기반(`m_i2c_as7341_dev_t`)으로 리팩터링해서 NIR1/NIR2가 동일 코드를 공유하도록 변경.
  `nirs_sample_t.raw`를 `[NIR_SENSOR_COUNT][NIRS_WAVELENGTH_COUNT]`로 확장(센서별 raw 분리 보존).
- 1차 실기 검증: NIR1은 정상, **NIR2는 간헐적으로 I2C 통신 실패**(`i2c err=-5`, EIO/NACK,
  4회 중 2회). devicetree/핀 설정은 컴파일된 `zephyr.dts`로 직접 확인해 정상임을 검증
  (UART0 비활성화로 P0.07/P0.08 경합 없음, i2c0_default psels가 SDA=P0.07/SCL=P0.08로
  정확히 반영됨) — 배선 문제라면 100% 재현돼야 하는데 확률적으로 실패해서 배선/납땜
  결함이 아니라고 최종 판단.
- **근본 원인 확정 및 수정**: `nrfutil device reset`(MCU 소프트 리셋)은 VDD1.8V를 유지한 채
  MCU만 재시작한다 — 직전 I2C 트랜잭션이 리셋으로 중간에 끊기면 AS7341이 SDA를 물고 있는
  상태로 남아 다음 START가 간헐적으로 NACK난다(전형적인 I2C 버스 stuck). `m_i2c_as7341_init()`의
  WHOAMI 읽기를 `i2c_recover_bus()`(SCL 클럭킹으로 슬레이브 해제) + 최대 3회 재시도로 감싸서
  해결 — 재검증 결과 NIR1/NIR2 모두 6회 연속 성공.

## v0.0.8 (2026-09-15)

- **RTC 100ms ISR 동작 실측 검증** (트래커 "Zephyr Task RTC Timer로 100ms ISR 구현" 항목).
  `m_i2c_rtc.c`(nrfx RTC2 + Bresenham 보정)는 이미 구현/버그수정 완료 상태였으나, 태스크
  측(`m_i2c_task_entry`)에서 세마포어로 깨어나는 시점의 RTC 타임스탬프를 임시 로그로
  확인(`TEMP_RTC_TICK_LOG_TEST`) — RTT 로그의 실제 벽시계 시간 간격이 매 tick마다
  99.98~100.03ms로 지터 0.03ms 이내임을 확인, `m_i2c_rtc_get_timestamp_us()` 누적값도
  정확히 100000us씩 증가함을 확인. 검증 후 임시 로그 비활성화(`TEMP_RTC_TICK_LOG_TEST=0`).

## v0.0.9 (2026-09-15)

- **[버그 수정] RTC 5-frame Bresenham 보정 비율 반전** (트래커 "5프레임 합=13381 tick=500ms
  산술 확인" 코드 리뷰 항목에서 발견). `next_cc_delta()`가 5프레임 중 1회만 HIGH(3277)+4회
  LOW(3276)를 쓰고 있었는데, 합계가 `1*3277+4*3276=16381` tick으로 목표(500ms=`32768*0.5`
  `=16384` tick)보다 3 tick(91.55µs) 부족했다. 100ms=3276.8 tick의 소수부가 0.8이므로 올바른
  비율은 **5프레임 중 4회 HIGH + 1회 LOW**(`4*3277+3276=16384`, 정확히 일치)여야 하는데 반대로
  구현돼 있었음 — 약 183ppm(시간당 약 0.66초) 만큼 `timestamp_us`가 실제 RTC2 크리스탈 기준
  경과시간보다 빠르게 누적되는 오차였다. `next_cc_delta()`의 HIGH/LOW 분기를 반전해서 수정.
  (참고: 이전 세션 코드 주석의 "13381 tick" 표기도 `16381`의 오기였음 — 그 숫자로도 500ms가
  안 나오므로 애초에 주석 자체가 산술적으로 틀려 있었다.)

## v0.0.10 (2026-09-15)

- **[버그 수정] Ring buffer overflow 반환값이 최초 1회 발생 후 영구 고착**. 코드베이스
  전수 리뷰 중 발견: `m_i2c_ring_buffer_push()`가 누적 카운터(`g_dropped_sample_count`,
  init 시에만 리셋)를 기준으로 `MODULE_ERR_RING_BUFFER_OVERFLOW` 반환 여부를 판정하고
  있어서, **최초 1회 overflow가 발생한 이후로는 실제로 overflow가 아닌 정상 push 호출도
  전부 영구히 OVERFLOW를 반환**하고 있었다 — `m_i2c.c`가 매 샘플마다 `m_ctrl_report_error()`를
  호출해 ctrl_msgq를 계속 흘려보내는 부작용. 로컬 `bool overflowed` 플래그로 "이번 호출에서
  실제 overflow가 있었는지"만 반환하도록 수정. (설계 문서/헤더 주석이 원래 의도한 동작과
  실제 구현이 불일치했던 케이스 — 코드 리뷰로 발견)
- 코드베이스 전수 검토(RTC/AS7341/LED/ring buffer/CTRL/BLE) 완료. 아래 2건은 버그는
  아니나 설계상 주의가 필요해 architecture.md §11에 open-item으로 기록 예정:
  1. AS7341 STATUS2.AVALID 폴링 타임아웃(최대 200ms) × NIR1+NIR2 순차 = 최악 400ms인데
     RTC tick 주기는 100ms이고 `sem_i2c_ready`가 binary(max count 1)라, I2C가 느려지거나
     멈추면 세마포어가 소비되지 못한 tick을 조용히 잃어버릴 수 있음 — 이 경우 로그/에러
     보고가 없음(현재는 `m_i2c_as7341_read_raw()`가 실제 I2C 에러를 반환할 때만 보고됨).
  2. AS7341 SP_EN을 매 tick마다 재기록하지만 datasheet SPM 모드는 free-running이라, LED
     on 구간과 실제 적분 구간이 정확히 일치한다는 보장이 없음 — 기존 TODO(LED 시퀀싱
     정책 미확정)와 같은 근본 원인이라 별도 항목으로 새로 만들지 않고 해당 TODO에 통합.
  3. (참고, 코드 아님) NIR2 추가로 `nirs_sample_t.raw`가 2배로 커져 BLE batch 크기
     추정치(architecture.md A3, "샘플당 약 16~20byte" 가정, `BLE_BATCH_MAX_SAMPLES=12`)가
     더 이상 맞지 않음 — Rev2(R2-1) 착수 전 재계산 필요.

## v0.0.11 (2026-09-15)

- **BLE 실 스택 착수** — 테스트 APK(`sw/fnirs-visualizer-app-release.apk`) 호환 연동이 목표.
  파일 트리는 S2(NCS_TedreamS2) 관례 계승: `m_ble.c`(태스크/광고/연결) / `m_ble_gatt.c`(GATT
  서비스·characteristic 선언) / `m_ble_proto.c`(패킷 인코드·디코드) 3분할 신규 추가.
  프로토콜 바이트 포맷 자체는 S2가 아니라 APK가 요구하는 고정 스펙을 따른다.
- APK `classes.dex` 문자열 분석(jadx/apktool 없이 grep -a로 진행)으로 확인한 값 반영:
  기기명 `nRF_fNIRS_Sys_v2`, 서비스 `AS7341_SERVICE_UUID`(00001523-1212-efde-1523-785feabcd123),
  characteristic `AS7341_CONFIG/DATA0/DATA1_UUID`(0x1525/1526/1527, sensor0/1=NIR1/NIR2 대응).
  TODO(open-item): 4개 UUID 값과 4개 이름의 정확한 매핑, CONFIG write의 바이트 레이아웃은
  아직 미검증 — `m_ble_proto_on_config_write()`가 raw bytes를 로그로 남기도록 구현해둠
  (실기+APK 연동 시 LED 슬라이더 조작하며 캡처해서 역설계 예정).
- Zephyr BLE peripheral 활성화(`CONFIG_BT`, `bt_enable()`, `AS7341_SERVICE_UUID` advertising),
  connected 콜백에서 `m_ctrl_notify_ble_connected()` 연동.
- **[버그 수정] I2C/BLE 초기화 순서 경합**: `bt_enable()`이 AS7341(NIR1) SMUX 폴링 도중
  (`k_sleep(1ms)` 구간)에 끼어들면 I2C1 트랜잭션이 멈추는 문제를 실기에서 확인(재현율 100%,
  RTT 로그가 WHOAMI 이후 진행 없이 멈춤). I2C Task가 AS7341 초기화(성공/실패 무관) 완료 후
  `sem_i2c_init_done`을 주고, BLE Task는 `bt_enable()` 전에 이 신호를 기다리도록 수정 — 이후
  NIR1/NIR2 모두 3/3 재현 성공. 추가로 RTC 시작 시점도 BLE `bt_enable()` 완료 후로 미뤄서
  (`sem_ble_init_done`) RTC2의 100ms 인터럽트가 BLE 스택 초기화 도중 계속 발생하지 않게 했다.
- **[진단, 버그 아님으로 결론]** 수정 후에도 RTT 로그가 "AS7341 초기화 완료" 직후 특정
  지점에서 계속 멈추는 현상을 추가 조사 — J-Link로 CPU를 직접 halt해서 fault handler
  미도달·CycleCnt 정상 진행(=idle 상태)을 확인했고, 결정적으로 **Zephyr 표준 `samples/bluetooth/beacon`
  예제를 동일 보드·툴체인으로 빌드해도 동일한 지점 근처에서 RTT 캡처가 끊기는 것**을
  확인 — 우리 코드 결함이 아니라 `bt_enable()`이 초기 로그를 대량 출력하는 구간에서
  이 환경의 JLinkRTTLogger가 못 따라가는 캡처 툴 한계로 결론. **실기 BLE advertising 최종
  성공 여부는 RTT로 확인 불가 — 폰(nRF Connect 등)으로 `nRF_fNIRS_Sys_v2` 스캔 확인이
  필요(미검증 상태로 남김)**.

## v0.0.12 (2026-09-16)

- **DATA0/DATA1 실 notify 구현** — v0.0.11까지는 ring buffer를 소비만 하고 버리는
  S2식 batch placeholder였음(`send_batch()`가 no-op). `m_ble.c`에서 샘플을 pop할
  때마다 `m_ble_proto_encode_sample()` + `m_ble_gatt_notify_data0/1()`로 즉시
  NIR1→DATA0/NIR2→DATA1 notify하도록 교체. 테스트 APK 프로토콜은 배치가 아니라
  샘플 1개당 notify 1회를 기대하므로 S2 batching은 이번 마일스톤에는 적용하지 않음
  (`m_ble_batch.c/h`는 삭제하지 않고 Rev2 프로덕션 프로토콜용으로 유지, 현재는 미사용).
  notify 실패는 §6(silent 버림 금지)에 따라 `s_notify_drop_count`(실전송 실패)/
  `s_notify_skip_count`(미연결·미구독 정상 상태)로 구분 카운트.
- `TEMP_AS7341_RAW_DBG_LOG` 플래그 추가(config_app.h) — `m_i2c_as7341.c`의 기존
  `LOG_DBG(F5~NIR raw)`를 앱 연동 중에도 RTT로 확인할 수 있게 로그 레벨 상향.
  **검증 완료**: RTT 로그 값과 앱(Logcat `fNIRS_RAW`) 수신 hex/decoded 값이 정확히
  일치함을 실기에서 확인 — 인코딩/프로토콜 정합성 검증 완료.
- **[버그 수정] BLE 연결 해제 후에도 계속 센싱하던 문제** — `m_ctrl_notify_ble_disconnected()`
  신규 추가(`m_ctrl.h/c`), `m_i2c.c`에 `reset_to_device_on()` 추가해 BLE_BLINK/ACQUISITION
  상태에서 매 tick `m_ctrl_is_ble_connected()`를 확인, 연결 끊기면 즉시 측정을 멈추고
  LED 순차점등(디바이스 On) 상태로 복귀 + `seq_num` 리셋. 실기 검증 완료(LED 정상 복귀).
- **[버그 수정] 연결 해제 후 advertising이 재개되지 않던 문제** — Zephyr peripheral은
  연결되면 advertising이 자동 중단되고 해제 후 자동 재시작되지 않음. 최초 수정(disconnected
  콜백에서 `bt_le_adv_start()` 직접 호출)은 LED는 복귀하지만 광고는 안 살아나는 증상이
  실기에서 재현됨 — SoftDevice Controller가 disconnect HCI 이벤트를 마무리하는 도중이라
  광고 시작 명령이 조용히 실패하는 것으로 추정. 시스템 워크큐(`k_work_submit`)로 재시작을
  한 틱 미루도록 수정 후 **실기 재검증 완료 — 연결 해제 즉시 앱에서 재스캔 가능**.
- **테스트 Android 앱(`fNIRS_Android_Kotlin`, 별도 저장소) 동시 수정**:
  - DATA0/DATA1 수신 raw hex+decoded 값을 Logcat(`fNIRS_RAW`)과 화면 하단 "Raw Notify
    Log" 탭(1526/1527 전환)으로 확인 가능하도록 추가.
  - **[버그 수정] 재연결 시 AS7341 Controls 값이 기기 기본값으로 보이던 문제**: 매 연결마다
    CONFIG characteristic을 무조건 읽어서 UI를 덮어쓰던 로직을, 앱 최초 연결 시에만 읽고
    이후 재연결부터는 앱이 마지막으로 알던 설정을 기기에 다시 쓰도록 변경.
  - **[버그 수정] Start CSV가 실제로는 저장 안 되던 문제**: 공용 Documents 폴더에 File
    API로 직접 쓰던 방식이 Android 10+ scoped storage에서 조용히 실패 — 최초엔 앱 전용
    외부 저장소(`getExternalFilesDir`)로 우회했다가, 사용자 요청으로 **MediaStore API
    기반 `Documents/fnirs_rawdata/` 공용 경로**로 재변경(권한 요청 없이 공용 폴더 쓰기
    가능, API29 미만은 기존 File API로 폴백). 파일명 `{yyyy-MM-dd_HH-mm-ss}_fNIRS_RAW_DATA.csv`.
    adb로 실기 저장 파일 4개(정상 헤더+데이터) 확인해 정상 동작 재검증 완료.
- **실기 검증 완료**: 앱 연동 BLE advertising/연결/재연결, CONFIG write→LED PWM 제어,
  DATA0/DATA1 raw data 품질(RTT 대조 일치) — v0.0.11에서 미검증으로 남겼던 항목들 전부 해소.
  파장별 정량 캘리브레이션은 레퍼런스 데이터 미확보로 계속 보류(architecture.md A12).
- (참고, 코드 아님) 로컬 개발 환경(NCS 툴체인 Python)에서 Windows 레지스트리의 별도
  Python 3.12 설치 `PythonPath` 키가 cmake reconfigure 시 `ctypes`를 깨뜨리는 문제를
  발견 — 해당 레지스트리 값 제거로 해결(다른 Python 설치의 정상 실행에는 영향 없음,
  이 저장소 코드와 무관한 로컬 머신 설정 이슈).

## v0.0.13 (2026-09-16)

- **[하드웨어 갭 기록] 온도센서/배터리 IC 부재** — PoC v1 스키마틱(`docs/SCH_fNIRS_Sleep_Project.pdf`)
  검토 결과, Thermal 보호(§7)와 배터리 잔량 모니터링(§8)에 필요한 하드웨어가 아예 없음을 확인:
  온도센서(NTC/디지털) 미실장, fuel gauge/monitor IC 미실장, 게다가 nRF52832의 ADC 가능 핀
  (P0.02~P0.05/AIN0~AIN3)이 전부 LED 제어 신호로 이미 점유돼 있어 VBAT 저항분배를 붙일 여유
  ADC 핀도 없음. 소프트웨어로 우회 불가능한 순수 하드웨어 제약이라 architecture.md §7/§8/§11에
  경고 문구와 "다음 보드 리비전 반영 예정" 항목으로 기록. Rev3(Reliability) 범위에서 Thermal/
  Battery 항목은 제외하고 Watchdog만 우선 진행하기로 결정.
- **Watchdog 구현 (R3-1)** — nRF52832 내장 WDT(`wdt0`, 보드 dts 기본 `status="okay"`라
  devicetree overlay 변경 없이 `CONFIG_WATCHDOG=y`만으로 사용 가능)를 `m_ctrl.c`가 소유.
  `m_i2c`/`m_ble` Task가 각자 메인 루프 끝에서 `m_ctrl_notify_alive(CTRL_ALIVE_I2C/BLE)`로
  생존 신호를 보내고, `m_ctrl` Task는 `K_MSEC(WATCHDOG_CHECK_PERIOD_MS=500)` 주기로 깨어나
  두 소스 모두 `WATCHDOG_ALIVE_STALE_MS(1000ms)` 이내에 응답했을 때만 `wdt_feed()`를 호출한다
  — 한쪽이라도(예: I2C 버스 hang) 멈추면 feed가 끊겨 `WATCHDOG_TIMEOUT_MS(4000ms)` 후 SoC
  전체가 자동 리셋된다(`WDT_FLAG_RESET_SOC`). 타임아웃 값은 AS7341 STATUS2 폴링 최악 케이스
  (NIR1+NIR2 순차 최대 400ms, v0.0.7 CHANGELOG 참고)에 여유를 둔 값 — 실측 후 조정 가능.
  CTRL 태스크의 에러 메시지 큐 처리(`k_msgq_get`)도 기존 `K_FOREVER`에서 같은 주기의
  `K_MSEC` 타임아웃으로 바꿔 watchdog feed 체크와 한 루프에서 같이 돈다.
- **빌드 검증 완료** — FLASH 62.66%/RAM 59.66%. **하드웨어 검증 미실시** — 실기에서 I2C
  버스를 의도적으로 막아 watchdog 리셋이 실제로 발생하는지(fault injection) 확인 필요.

## v0.0.14 (2026-09-17)

- **Safety 인증(IEC 60601-1/-1-8) 대응 필수 펌웨어 안전기능 6건 구현** (architecture.md
  §11 항목6, HW 제약 없는 소프트웨어 항목만 — 배터리/온도 관련 4건은 HW 미실장으로 계속 보류).
  1. **[안전상태 전이]** `m_ctrl_is_safe_state()` 신규(`m_ctrl.h/c`) — `CTRL_STATE_DEGRADED`/
     `FAULT`일 때 true. `m_i2c.c` tick 루프 맨 앞에서 체크해서 true면 LED 소등 + 측정/
     ring buffer push를 완전히 건너뛴다. **설계상 래치**(자동복구 없음, 재부팅으로만 해제) —
     IEC 60601 단일고장 안전 철학에 따른 의도적 선택.
  2. **[Watchdog 안전 재시작]** `m_ctrl.c`에 `log_reset_cause()` 추가, `CONFIG_HWINFO=y`
     (`prj.conf`)로 RESETREAS 레지스터를 읽어 직전 리셋이 watchdog이었는지 로그로 남기고
     `hwinfo_clear_reset_cause()`로 정리. 이 프로젝트는 원래 리셋 원인과 무관하게 항상
     안전한 기본 상태(`I2C_LED_MODE_DEVICE_ON`)로 부팅하므로 별도 상태 복원 로직은
     불필요 — 이번 변경은 "왜 리셋됐는지"를 사후 분석 가능하게 로그로 남기는 것.
  3. **[BLE 끊김 로컬 버퍼링 + 저전력 대기]** **버그 수정**: `m_ble.c`가 연결 끊김 중에도
     ring buffer를 계속 pop해서 `-ENOTCONN`으로 즉시 버리고 있었음(사실상 버퍼링이 전혀
     안 되던 상태) — 연결 상태일 때만 pop하도록 수정. `m_i2c.c`는 연결이 끊겨도
     `BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS`(300 tick=30초, 조정 가능) 동안은 즉시 멈추지
     않고 계속 측정+버퍼링하다가, 그 이상 끊겨 있으면 신규 `I2C_LED_MODE_STANDBY`(LED
     소등 + 2초마다 짧은 점멸로 대기 표시, 측정 중단으로 전력/발열 절감)로 전환. 재연결 시
     기존 10회 점멸 경로로 복귀 후 측정 재개. `reset_to_device_on()`의 `s_seq_num = 0`
     초기화 제거 — seq_num을 부팅 세션 내내 단조증가로 유지해야 gap 식별(아래 4번)이 가능.
  4. **[Sanity check + gap 식별]** `m_i2c.c`에 `check_sensor_sanity()` 추가 — module_err.h에
     이미 정의돼 있었지만 지금까지 아무도 판정하지 않던 `MODULE_ERR_SENSOR_SATURATION`/
     `MODULE_ERR_SENSOR_LOW_SIGNAL`을 실제로 채운다(고정 임계값, `config_app.h`
     `AS7341_SATURATION_THRESHOLD`/`AS7341_LOW_SIGNAL_THRESHOLD` — 정확한 채널별/게인별
     풀스케일 계산은 기존 gain/ATIME 캘리브레이션 open-item과 함께 추후 재확정).
     Gap 식별은 wire 프레임 변경 없이 위 3번의 seq_num 단조증가로 대체(앱이 seq_num
     불연속을 감지하면 로컬 버퍼 오버플로우로 유실된 구간). CRC/체크섬은 8바이트 고정
     프레임(테스트 APK 키건)에 추가할 여유가 없어 보류 — BLE 링크레이어 CRC24로 전송
     무결성이 이미 확보된다고 문서화(architecture.md §11).
  5. **[BLE 버전 characteristic 신규, 보안은 설계만]** AS7341_VERSION(0x1528, read-only)
     characteristic 신규 추가(`m_ble_gatt.c/h`) — `{fw_version, protocol_version(=1,
     m_ble_proto.h `BLE_PROTOCOL_VERSION`)}` 4바이트 반환. 순수 추가라 기존 CONFIG/
     DATA0/DATA1과 테스트 APK 호환성 영향 없음. **정정**: 이전 architecture.md에
     "펌웨어 버전 식별 기능 구현 완료"로 잘못 기록했었음 — 실제로는 `fw_version`이
     `nirs_sample_t`에만 있고 BLE로 전송되지 않았음(`m_ble_proto_encode_sample()`이 raw
     3값+led_index만 인코드), 이번에 이 characteristic으로 바로잡음. BLE 페어링/본딩/
     암호화(`CONFIG_BT_SMP`/`BONDABLE`)는 **비활성 상태로 `prj.conf`에 주석만 준비** —
     테스트 APK가 본딩을 지원하는지 불명해서 활성화 시 기존 검증된 연동이 깨질 위험이
     있어, 앱 쪽 확인 후 활성화하기로 함(architecture.md §11).
  6. **[OTA 인프라]** `prj.conf`에 `CONFIG_MCUMGR`/`CONFIG_MCUMGR_TRANSPORT_BT`/
     `CONFIG_IMG_MANAGER` 등 활성화(기존 주석 처리된 TODO 실행). **`pm_static.yml`은
     추가하지 않음** — 계획 단계에서는 신규 작성 예정이었으나, CHANGELOG v0.0.1/v0.0.11
     기록을 재확인한 결과 sysbuild 자동 파티션 매니저가 이미 MCUboot 2-image 빌드를
     정상 생성해온 이력이 있어(예: App Flash 32564B/216752B, mcuboot Flash 34768B/48KB),
     하드웨어 검증 없이 손으로 파티션 오프셋을 새로 지정하는 것이 CLAUDE.md §4/§14가
     경고하는 고위험 변경(브릭 위험)을 오히려 키운다고 판단 — 자동 파티셔닝에 맡기는
     쪽으로 계획을 변경함. 서명키는 sysbuild 기본 자동생성 디버그 키 사용(프로덕션 키
     관리는 별도 범위).
  - **검증 구분(§17)**: 코드 리뷰 완료, **빌드 검증 미실시**(이 세션 환경에 west/NCS
    툴체인이 없어 실행 불가 — 다음 실제 빌드 세션에서 clean build 확인 필요),
    하드웨어 검증 전부 미실시(watchdog RESETREAS 로그, BLE 강제 끊김→재연결 버퍼 flush,
    STANDBY 점멸 육안 확인, OTA 실기 업데이트/롤백/강제전원차단 3-시나리오 전부 미검증).

## v0.0.15 (2026-09-17)

- **빌드 환경 확인**: `C:\ncs\toolchains\`에 NCS v3.4.0 툴체인이 로컬에 설치돼 있음을
  확인 — `west build -b nrf52dk/nrf52832 Application`(sysbuild) clean build 성공.
  FLASH 152700B/216752B(70.45%), RAM 44152B/64KB(67.37%), `dfu_application.zip`
  생성 확인(OTA 페이로드 실제 산출물).
- **[버그 수정] MCUMGR Kconfig 오류(v0.0.14에서 유입)**: `CONFIG_MCUMGR_SMP_BT_AUTHEN`은
  존재하지 않는 심볼(작성자가 잘못 지어낸 이름)이라 Kconfig 경고로 빌드 자체가
  중단됐음 — 제거. `CONFIG_MCUMGR`가 `CONFIG_ZCBOR`에 의존하는데 누락돼 있었음 —
  `CONFIG_ZCBOR=y` 추가. `BT_SMP` 비활성 상태에서는 `MCUMGR_TRANSPORT_BT_PERM`
  choice가 자동으로 암호화 불필요 옵션(`_RW`)을 선택하므로 별도 설정 불필요함을 확인.
- **실기 플래시 검증**: MCUboot+App 2-image 플래시 완료(J-Link, `nrf52dk/nrf52832`
  타겟). 1차 시도에서 App 이미지 검증 실패(주소 0x0000c000 mismatch) — 점퍼 연결
  불안정성으로 추정, 재시도 후 mcuboot/App 둘 다 검증 통과.
- **[버그 수정, 중대] BLE notify 미구독 상태를 통신 실패로 오분류하던 문제**: RTT 실기
  로그에서 `BLE notify 실패(sensor=X, err=-22)`가 반복 발생하는 것을 발견 — 앱이 실제로
  연결돼 있었는데도 나타남. Zephyr 소스(`subsys/bluetooth/host/gatt.c` `gatt_notify()`)
  확인 결과 `bt_gatt_notify()`의 `-EINVAL`은 "해당 characteristic에 아직 notify
  구독(CCC) 안 됨"을 뜻하는 정상 상태이지 통신 실패가 아님을 확인(`-ENOTCONN`만
  실제로 "연결 안 됨"). 기존 코드(v0.0.12부터)는 `-ENOTCONN` 외의 모든 에러를 진짜
  실패로 취급해 `MODULE_ERR_BLE_TX_FAILED`를 보고하고 있었음 — 이 자체는 이전까지는
  무해했으나(DEGRADED 상태를 아무도 소비하지 않았음), v0.0.14의 `m_ctrl_is_safe_state()`
  도입으로 **앱이 연결 직후 아직 구독하기 전인 정상적인 과도 상태에서 측정을 영구
  중단시키는 회귀**로 이어질 뻔했음. `m_ble.c notify_sample()`이 `-EINVAL`도
  `-ENOTCONN`과 동일하게 정상(skip) 처리하도록 수정.
- **검증 구분(§17)**: 빌드 검증 완료, 실기 플래시 검증 완료. **BLE notify EINVAL 수정
  실기 재검증 완료** — 재빌드/재플래시 후 앱 연결 상태로 34초 연속 RTT 캡처, notify
  실패 경고 0건, DATA0/DATA1 notify enabled 확인, NIR1/NIR2 raw 데이터 100ms 간격
  연속 스트리밍 확인. `log_reset_cause()`도 실기에서 동작 확인(RESETREAS에 watchdog
  비트 포함된 것을 감지+로깅). Watchdog fault injection, STANDBY 점멸, OTA 실기
  업데이트/롤백/강제전원차단 3-시나리오는 여전히 미검증.

## v0.0.16 (2026-09-17)

- **[TEMP, OTA 실기 테스트용]** `TEMP_OTA_TEST_MARKER=1`(config_app.h) 추가 — 부팅
  직후(디바이스 On 순차점등 진입 전) LED 3개 동시 5회 빠른 점멸(`m_i2c.c i2c_init()`).
  OTA로 이 이미지가 실제로 올라갔는지 기존 이미지(v0.0.15, 순차 점등만 함)와 육안으로
  바로 구분하기 위한 용도 — **OTA 테스트 완료 후 반드시 0으로 되돌릴 것**(TODO 아님,
  영구 기능 아님). 기능 변경 없음, 오직 이 목적의 임시 마커.
- **[버그 수정, 중대] OTA 업데이트가 "버전 체크"로 거부되던 문제**: nRF Connect Device
  Manager에서 Start를 눌러도 업데이트가 진행되지 않는 문제 발생 — `dfu_application.zip`의
  `manifest.json`을 열어 확인한 결과 `version_MCUBOOT: "0.0.0+0"`으로 찍혀 있었음.
  `config_app.h`의 `FW_VERSION_*`(앱/BLE characteristic용)와 MCUboot 이미지 헤더 버전은
  완전히 별개 체계인데, 프로젝트에 `Application/VERSION` 파일이 없어서 **지금까지의
  모든 빌드가 전부 버전 `0.0.0+0`으로 찍히고 있었음** — 현재 보드의 이미지도, 새로
  올리려던 이미지도 버전이 동일해서 MCUboot/Device Manager가 업데이트로 인정하지
  않은 것으로 추정. `Application/VERSION` 신규 추가(`PATCHLEVEL=16`), pristine
  재빌드 후 `version_MCUBOOT: "0.0.16+0"`으로 정상 반영 확인.
  **운영 규칙(신규)**: 앞으로 패치마다 `config_app.h`의 `FW_VERSION_PATCH`와
  `Application/VERSION`의 `PATCHLEVEL`을 **함께** 올려야 한다 — 빌드 시스템이 두
  값을 자동으로 동기화해주지 않는다.
- **[버그 수정, 중대] OTA 업로드 시작 시 "GATT ERROR"로 실패하던 문제**: 버전 이슈
  해결 후에도 nRF Connect Device Manager에서 Start 시 `State: GATT ERROR`로 실패
  (SMP Service: DISCONNECTED). 원인: `CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY`를
  켜지 않은 채였음 — 기본 ATT MTU(23byte)로는 SMP 업로드 요청이 여러 조각으로
  나뉘는데, 재조립(reassembly) 없이는 조각난 요청을 처리 못 해 GATT 에러로
  이어짐. NCS가 제공하는 공식 검증 Kconfig 번들
  (`nrf/samples/common/mcumgr_bt_ota_dfu/Kconfig`의
  `NCS_SAMPLE_MCUMGR_BT_OTA_DFU`/`..._SPEEDUP`)로 `prj.conf`의 수작업 MCUMGR
  설정을 전부 교체 — reassembly, 버퍼 크기(`MCUMGR_TRANSPORT_NETBUF_SIZE=1230`),
  연결 파라미터 제어, MTU 확장(`BT_L2CAP_TX_MTU=247`, `BT_BUF_ACL_TX/RX_SIZE=251`)이
  한 번에 올바르게 설정됨. 이 번들은 build-time에 설정 정합성을 자체 검증하는
  옵션도 포함(`NCS_SAMPLE_MCUMGR_BT_OTA_DFU_VALIDATION`, 기본 활성).
- **검증 구분(§17)**: 빌드 검증 완료(image 버전 0.0.17+0, pristine rebuild 성공,
  FLASH 오버플로우 없음). **실기 OTA 업로드 재검증 필요**(GATT ERROR 수정 후
  아직 미확인) — 다음 실기 테스트에서 확인.
- **[중요 교훈] OTA는 "현재 이미 설치된 이미지가 OTA를 받을 능력이 있어야" 성립**:
  GATT ERROR 수정(reassembly 등) 반영한 v0.0.17을 만들어도, 정작 보드에 실행 중이던
  이미지(v0.0.15, reassembly 없음)가 업데이트 요청 자체를 받을 능력이 없어서
  계속 실패했음 — Device Manager의 "Buffer details: 1 x 20 bytes"가 결정적 단서
  (재조립 없는 구버전의 SMP 버퍼 크기와 정확히 일치). **v0.0.17을 유선으로 한 번
  플래시한 뒤에야** OTA 수신 능력이 생김. 이후 v0.0.18(LED 8회 점멸 마커로 변경,
  기존 v0.0.17의 5회 점멸과 구분)을 실제로 **무선 OTA로 업데이트 성공** —
  Device Manager Buffer details가 `4 x 2475 bytes`로 정상 확인(reassembly 반영),
  Bootloader: MCUboot / Swap Without Scratch, 업데이트 후 재부팅 시 LED 8회
  점멸 육안 확인 + RTT 로그로 `Board FW Version: v0.0.18` 확인. **OTA 실기 검증
  완료** (§17) — 최초 1회는 반드시 유선 플래시 필요, 이후 SMP 설정이 유지되는 한
  버전 상승 방향으로는 무선 업데이트 가능함을 확인. 롤백/강제전원차단 시나리오는
  아직 미검증.

## v0.1.1 (2026-09-17)

- **버전 체계 전환**: OTA 실기 검증 성공을 기점으로 `FW_VERSION_MINOR`를 0→1로 올려
  새 관리 기준선을 v0.1.1로 삼는다(`config_app.h`, `Application/VERSION`). 이후
  패치는 v0.1.1 → v0.1.2 → ... `TEMP_OTA_TEST_MARKER`는 정식 버전이므로 0으로 원복.
  v0.0.18(OTA 최초 성공 버전)에서 이 버전으로 OTA 업그레이드 실기 검증 완료 —
  1차 시도 GATT ERROR(재시도로 성공, BLE 연동 준비 지연으로 추정·설정 문제 아님),
  재시도 후 RTT 로그로 `Board FW Version: v0.1.1` 정상 확인.
- **[정책 결정] OTA 다운그레이드 실기 검증 및 의도적 허용**: MCUboot
  `check_downgrade_prevention()`은 `CONFIG_MCUBOOT_DOWNGRADE_PREVENTION`이 켜져
  있을 때만 동작하는데, 현재 빌드에는 이 옵션이 없음을 `.config`로 확인 —
  즉 지금은 낮은 버전으로도 OTA가 그대로 적용된다. v0.1.1에서 v0.1.0(다운그레이드
  테스트용 임시 이미지, 실사용 릴리스 아님, LED 3회 점멸 마커)으로 실제 다운그레이드
  실기 검증 완료(RTT: `Board FW Version: v0.1.0`). **사용자 결정(2026-09-17)**:
  인증/테스트 단계에서는 다운그레이드를 의도적으로 열어둔다(펌웨어를 이전 버전으로
  되돌려 비교/재현해야 하는 경우가 있음) — **양산 배포 시점에 `CONFIG_MCUBOOT_
  DOWNGRADE_PREVENTION`을 활성화하는 것은 사용자가 별도로 진행**하기로 함
  (architecture.md §11에 기록).
- 다운그레이드 테스트 후 코드 상태는 다시 v0.1.1(정식 기준선)로 원복. 보드는
  v0.1.0 상태이므로 **다음 작업 전 v0.1.1로 다시 OTA 업그레이드 필요**.
- 사용자가 v0.1.0→v0.1.1 OTA 업그레이드 재실기, RTT로 `Board FW Version: v0.1.1`
  정상 확인.

## v0.1.2 (2026-09-17)

- **[신규] 부팅 버전 점멸 표시** — SWD/RTT 연결 없이(조립된 상태) 육안으로 OTA
  적용 여부를 확인하고 싶다는 요청으로 추가. 오늘 쓰던 `TEMP_OTA_TEST_MARKER`를
  대체하는 정식 기능으로 승격: 부팅 직후(디바이스 On 순차점등 진입 전) LED 3개가
  동시에 `FW_VERSION_PATCH + 1`회 점멸한다(`config_app.h` `FW_VERSION_BOOT_BLINK_MS`,
  `m_i2c.c i2c_init()`). +1은 PATCH=0일 때 "0회 점멸"(표시 없음)이 되는 것을 피하기
  위함 — v0.1.1은 2회, 이 버전(v0.1.2)은 3회 점멸. 업데이트 전/후 점멸 횟수 변화만
  보면 OTA 성공 여부를 육안으로 판단할 수 있다.
- **[정정] "seq_num 기반 gap 식별 가능"은 과장된 주장이었음** — `m_ble_proto_
  encode_sample()`의 DATA0/DATA1 8바이트 프레임에는 raw 3값+led_index만 들어가고
  `seq_num`은 애초에 전송되지 않는다(이전부터 그랬음, 오늘 seq_num 리셋을 없앤 것과
  무관하게 앱은 이 값을 볼 방법이 없음). architecture.md §11 항목6-[4] "gap 식별
  방식 확정"을 "인프라 미비, 실질적 gap 식별 불가"로 정정 필요 — TODO(open-item):
  AS7341_VERSION(0x1528)처럼 seq_num을 노출하는 추가 characteristic이 필요하다
  (기존 DATA0/DATA1 프레임 변경은 테스트 APK 호환성 문제로 비권장).
- **[실기 검증 완료] OTA 강제 전원차단 시나리오**: 업데이트 전송 도중 디바이스 전원을
  끊으면 MCUboot가 손상된 이미지로 스왑하지 않고 원래(이전) 버전으로 안전하게
  부팅 롤백됨을 확인. 재부팅 후 BLE도 자동으로 재연결되어 OTA를 이어서 진행할 수
  있음을 실기로 확인 — "Swap Without Scratch" 방식의 원자적 스왑 안전성이 실제로
  보장됨을 검증. 남은 미검증 항목은 "의도적으로 손상시킨(서명 불일치) 이미지
  업로드 시 롤백" 시나리오뿐.
- **[버그 수정] gap 식별용 seq_num이 실제로는 앱에 전달되지 않던 문제** — 신규
  AS7341_SEQ(0x1529, notify-only, u32 LE) characteristic 추가(`m_ble_gatt.h/c`,
  `m_ble_proto.c m_ble_proto_encode_seq()`, `m_ble.c notify_seq()`). DATA0/DATA1과
  같은 tick에서 함께 notify되므로, 앱은 최근 SEQ/DATA notify를 짝지어 seq_num
  불연속을 감지하면 그 구간이 로컬 buffer 오버플로우로 유실된 실측 구간임을 알 수
  있다. 순수 추가(additive)라 기존 CONFIG/DATA0/DATA1/VERSION 동작과 테스트 APK
  호환성에 영향 없음(앱이 이 characteristic을 구독하지 않으면 그냥 무시됨).
  **빌드 검증 완료, 실기 검증 필요**(테스트 APK가 이 characteristic을 아직 모르므로
  BLE 스니퍼 또는 nRF Connect 범용 앱으로 notify 값 확인 필요).
- **[실기 검증 완료] SEQ characteristic 실기 확인** — nRF Connect 범용 앱으로 구독,
  100ms 간격으로 notify 값이 올라옴을 확인. **설계적 한계 발견**: BLE Notification은
  ATT 레벨에서 ACK가 없는 fire-and-forget이라, 관찰상 불규칙하게(+2 정도) 값이
  건너뛰는 현상이 있었음 — RTT로 `RING_BUFFER_OVERFLOW`/`BLE notify 실패` 로그가
  전혀 없었던 것으로 볼 때, 이는 진짜 데이터 유실이 아니라 SEQ notify 패킷 개별
  전달 실패(무해)로 판단됨. 이 방식은 "진짜 gap"과 "notify 전달 실패"를 구분하지
  못하는 한계가 있음(architecture.md §11 항목6-[4] 기록).

## v0.1.3~v0.1.4 (2026-09-17)

- **[TEMP, watchdog 실기 fault injection 테스트용]** `TEMP_WATCHDOG_FAULT_INJECT_TEST`
  추가 — 부팅 후 20번째 tick(약 2초)에서 `m_i2c` 태스크가 고의로 무한 대기하며
  `m_ctrl_notify_alive()` 호출을 멈춘다. **실기 검증 완료**: RTT로 연속 캡처한 결과
  ①경고 로그 발생(t≈3.2s) → ②약 4초 후 하드웨어 watchdog이 SoC 강제 리셋 →
  ③재부팅 시 `log_reset_cause()`가 `RESETREAS=0x00000010`(watchdog 비트)을 정확히
  감지+경고 로그 — 이 사이클이 반복되는 것을 두 번 연속 확인. **item [2](Watchdog
  안전 재시작 시퀀스) 실기 검증 완료로 확정**.
- 테스트 완료 후 `TEMP_WATCHDOG_FAULT_INJECT_TEST`를 0으로 원복(v0.1.4) — 무한
  리셋 루프에서 정상 동작으로 복귀.

## v0.1.5 (2026-09-18)

- **주간과제4(BLE 실 스택+APK 연동 PWM 제어) 미달성 지점 해소: AS7341_CONFIG의
  cycle/active window(offset 6-9)를 실제 LED 구동에 반영**. 그동안 `m_ble_proto.c`가
  파싱·클램프만 하고 `m_i2c.c`는 duty(%)만 적용해서, 앱이 "1초 주기 중 200ms만
  점등"처럼 설정해도 실기에서는 duty%로 계속 켜져 있는 상태였다(`m_ble_proto.h`의
  TODO(open-item) 참고).
- `m_i2c.c`에 tick 기반 게이팅 추가: RTC 100ms tick 주기 자체(§2.3 Bresenham 보정)는
  건드리지 않고, ACQUISITION 모드에서 "이번 tick이 active 구간인지"만
  `s_gate_tick_count % s_gate_cycle_ticks < s_gate_active_ticks`로 판정한다.
  active 구간이 아니면 `acquire_one_sample()` 자체를 건너뛴다 — LED만 끄고 AS7341
  read를 계속하면 `check_sensor_sanity()`가 매 tick `LOW_SIGNAL`로 오판해
  `g_ctrl_status`를 영구 `WARNING`으로 전환시키는 부작용이 있어(다운그레이드 경로
  없음, `m_ctrl.c` 참고) acquire 자체를 스킵하는 방식을 택했다. `seq_num`은 push된
  샘플에만 증가하므로 gap 식별(SEQ notify, §11 항목6-4) 의미도 그대로 유지된다.
- 기본값은 cycle=1 tick/active=1 tick(항상 active, 게이팅 없음) — BLE가 CONFIG를
  한 번도 안 보낸 상태(`TEMP_AS7341_READ_TEST` 등)에서 기존 검증된 매 tick 측정
  동작을 그대로 보존하기 위함. `cycle=1000ms/active=200ms` 기본 프로토콜 값은 우리
  100ms RTC tick과 정수배(10 tick/2 tick)로 정확히 나누어떨어짐을 확인.
- TODO(open-item): 빌드/실기 검증 미실시(이 세션 환경에 west/NCS 툴체인 없음, §17).
  다음 실제 빌드 세션에서 clean build 확인 후 앱 연동 실측 필요.

## v0.1.6 (2026-09-18)

- **[코드 변경 없음, 문서만] v0.1.5 실기 검증 결과 정리**: cycle/active window 게이팅이
  실제로 정상 동작함을 RTT 로그로 확인 — `gate=1/1 tick`(cycle=100ms/active=20ms처럼
  1 tick 미만인 값)에서는 게이팅이 no-op이 되는 것도 예측대로 확인됨. `cycle=1000ms/
  active=200ms`로 재테스트 시 육안으로도 정상 동작 확인(사용자 확인, 2026-09-18).
- **BLE 연결 해제 후 LED가 즉시 순차 점등으로 안 돌아오는 것처럼 보인 문제 조사** —
  펌웨어 버그가 아니라 **테스트 APK가 disconnect 후 자동 재연결을 시도하지 않아서**
  항목6-[3]의 30초 grace period(`BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS`)를 매번 끝까지
  체감하게 되는 것으로 확인됨(advertising 자체는 disconnect 즉시 정상 재시작, RTT 로그
  확인). **펌웨어 동작은 그대로 유지하기로 결정** — architecture.md §11 항목7로 기록,
  `m_i2c.c`에 TODO(open-item) 주석 추가(앱 쪽 자동 재연결 로직 추가 필요, 펌웨어 범위 아님).

## v0.1.7 (2026-09-18)

- **오늘 할 일 중 앱 변경과 무관하게 FW 단독으로 안전한 항목만 적용**: BLE
  암호화(항목5-[4])는 architecture.md §11에 이미 "앱 본딩 지원 확인 전까지 보류
  확정"으로 기록돼 있어 제외, OTA 손상 이미지 롤백 검증은 코드 패치가 아닌 수기
  검증 절차라 제외. gap 식별 개선(항목5-[3])만 순수 추가 인프라로 적용.
- **AS7341_DROPPED_COUNT(0x152A, read-only, u32 LE) characteristic 신설**
  (`m_ble_gatt.h`/`m_ble_gatt.c`) — VERSION(0x1528)/SEQ(0x1529)와 동일한 순수 추가
  패턴, 구버전 앱 호환성 영향 없음. `m_i2c_ring_buffer_get_dropped_count()`가
  구현만 되고 아무도 호출하지 않던 죽은 코드였음을 발견해 여기 연결했다 — 앱이
  이 값과 SEQ 불연속을 대조하면 "진짜 ring buffer overflow"와 "SEQ notify 개별
  무해 유실"을 구분할 수 있다(실제 대조 로직은 앱 쪽 작업으로 남김).
- **[버그 수정] `m_i2c_ring_buffer_get_dropped_count()`/`get_max_usage()`가 mutex
  보호 없이 값을 읽고 있었다** — 지금까지 호출부가 없어서 문제가 드러나지 않았지만,
  이번에 BT 호스트 스레드(GATT read 콜백)에서 처음으로 cross-task 호출하게 되면서
  `mtx_ring_buffer`로 감싸도록 수정(codingstandard.md §8).

## v0.1.8 (2026-09-18)

- **[코드 변경 없음, 문서만] OTA 손상 이미지 롤백 케이스 A 실기 검증 완료**: 정상
  서명된 `zephyr.signed.bin`의 payload 중간 1바이트만 변조한 사본을 OTA로 업로드 →
  재부팅 후 `Board FW Version`/git hash가 업데이트 전과 완전히 동일(v0.1.5-
  45c7b088fdec)하고 I2C/BLE init도 전부 정상 — MCUboot가 서명 검증에서 손상된
  이미지를 거부하고 swap을 진행하지 않은 것으로 판단(MCUboot 자체 부팅 로그는
  캡처하지 못해 100% 확정은 아님, 정황 증거 기준). architecture.md §11 항목6-[6]에
  기록.
- **남은 케이스 B(서명 정상, confirm 미실행 시 자동 롤백)는 미검증** — west 빌드로
  구분 가능한 새 버전 이미지가 필요해서 이번 세션에서는 진행하지 못함.

## v0.1.9 (2026-09-18)

- **[코드 변경 없음, 문서만] OTA 케이스 B 시도 — 근거 불완전, 사용자 판단으로 pass
  처리**: v0.1.8을 test 업로드 후 confirm 없이 리셋해서 RTT 확인 시도. 1차 부팅
  (v0.1.8이어야 할 부팅)의 `Board FW Version` 배너가 RTT 캡처 과정에서 깨져서 안
  보였고, 2차 부팅만 `v0.1.5`로 깨끗하게 잡힘 — "1차에서 v0.1.8이 떴다가 롤백된 것"과
  "애초에 swap이 안 일어난 것"을 로그만으로 구분 불가. 사용자가 육안 관찰 근거로
  pass 처리했으나 **RTT 로그 상 명확한 증거는 없음** — architecture.md §11 항목6-[6]에
  근거 불완전 상태 그대로 기록. 재확인 필요 시 리셋 전 RTT 터미널을 미리 연결해서
  1차 부팅 배너부터 캡처해야 함.
- **후속(같은 날)**: Test 업데이트를 반복 실행하면서 매번 동일한 패턴(재부팅 후 이전
  버전으로 복귀)이 일관되게 재현됨을 확인 — 사용자 판단으로 케이스 B도 검증 완료
  (100%) 처리. architecture.md §11 항목6-[6] 갱신, 정식 자동화 TC는 양산 단계에서
  추가 예정.

## v0.1.10 (2026-09-18)

- **Safety 인증 대응 항목5-[2]/[3] 실기 검증 가시화용 로그 추가** (코드 리뷰로는
  완료 상태였으나 실기로 한 번도 트리거해본 적 없던 두 경로):
  - `enter_standby()`(`m_i2c.c`)에 STANDBY 진입 로그 추가 — 30초 grace period
    (`BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS`) 만료 시점을 RTT로 확인 가능하게 함.
  - ring buffer overflow 시(`m_i2c_ring_buffer_push()` 반환값 체크 지점) 누적
    dropped count를 포함한 경고 로그 추가.
  - `check_sensor_sanity()` 실패 시(SATURATION/LOW_SIGNAL) 어느 쪽인지 구분되는
    경고 로그 추가.
  - 세 로그 모두 동작 변경 없이 가시성만 추가한 것 — RING_BUFFER_CAPACITY(150)
    기준 10Hz 샘플링이면 약 15초 연결 끊기면 overflow, 30초면 STANDBY 진입이라
    한 번의 45초 연결 끊기 테스트로 두 경로를 동시에 확인 가능.

## v0.1.11 (2026-09-18)

- **[코드 변경 없음, 문서만] 항목5-[2]/[3] 실기 검증을 다음으로 연기하기로 결정** —
  SWD 연결이 불안정해서(9/17 watchdog 검증 때와 동일한 문제) RTT 로그 캡처 자체가
  너무 어려움. `JLinkRTTLogger.exe`로 파일에 직접 기록하는 방법을 안내했으나, SWD
  안정성 문제가 근본 원인이라 오늘은 검증을 보류. v0.1.10에서 추가한 로그는 그대로
  유효하니 SWD 환경이 안정적일 때 재시도하면 됨. architecture.md §11 항목6-[2]/[3]에
  연기 사유 기록.

## v0.1.12 (2026-09-21)

- **[버그 수정] AS7341 SP_EN(스펙트럴 엔진)을 매 tick마다 재기록하던 것을 init 시
  1회만 켜도록 정정** — `_ref_fnirs_example`(같은 AS7341 3파장 F7/F8/NIR 구성을 실사용
  검증한 레퍼런스, `src/as7341.c`)과 측정 방식을 대조하는 과정에서 발견. 레퍼런스는
  `as7341_init_device()`에서 SMUX 구성 직후 SP_EN을 1회만 켜고 이후 주기적 읽기
  (`as7341_read_current_mux_channels_device()`)에서는 STATUS2.AVALID 폴링과 채널
  읽기만 하고 ENABLE 레지스터를 다시 건드리지 않는다(datasheet가 문서화하는
  free-running SPM 모드 그대로). 우리 `m_i2c_as7341_read_raw()`는 매 호출(매 tick)마다
  `ENABLE=PON|SP_EN`을 재기록하고 있었음 — architecture.md §11 항목5의 미해결 우려사항
  ("SP_EN을 매 tick마다 재기록하지만 free-running 모드라 LED on 구간과 실제 적분 구간이
  정확히 일치한다는 보장이 없음", v0.0.10 기록)과 직결되는 부분. `m_i2c_as7341_init()`에
  SMUX 구성 성공 직후 SP_EN 1회 활성화를 추가하고, `read_raw()`의 재기록 코드는 제거해
  레퍼런스와 동일한 free-running 방식으로 정정. 불필요한 I2C 쓰기 1회/tick도 함께
  줄어듦(v0.0.10 기록된 NIR1+NIR2 순차 최악 400ms 폴링 부담에 소폭 도움).
- **[대조 결과] SMUX RAM 20바이트 설정값은 레퍼런스와 완전히 일치함을 확인** —
  `m_i2c_as7341.c`의 `s_smux_config_f5f8_clear_nir[20]`과 레퍼런스
  `as7341_setup_f5f8_clear_nir()`의 20바이트 값이 레지스터 0x00~0x13 전부 동일. 기존
  TODO(open-item, architecture.md §11 항목5 "공개 레퍼런스 구현 값을 우선 적용, ams 공식
  앱노트 교차검증 필요")의 교차검증 근거 하나가 추가로 확보됨(정식 앱노트 대조는 아니지만
  독립적인 실사용 레퍼런스와 값이 일치).
- **[구조적 차이, 패치 대상 아님] LED 구동 방식은 레퍼런스와 동일하지 않으나 하드웨어
  토폴로지 자체가 다름** — 레퍼런스는 RED/IR 2채널 LED를 2개 물리 위치(location)에
  배치해 놓고 위치 전환 타이머로 순환하며, LED는 BLE CONFIG로 받은 intensity로
  **계측 타이밍과 무관하게 상시 점등**(강도 제어만, 스트로빙 없음). 반면 이 프로젝트는
  640/680/950nm 3파장 LED가 한 위치에 개별 실장돼 있고(pinmap.md §3), 테스트 APK
  프로토콜(디컴파일 확인, CHANGELOG v0.0.11)이 요구하는 파장별 duty + cycle/active
  게이팅(v0.1.5)을 그대로 유지해야 함 — 레퍼런스의 "상시 점등" 방식을 이 프로젝트에
  그대로 옮기면 기존 검증된 APK 연동(CONFIG write→LED 제어)이 깨진다. 레퍼런스와
  구조가 다른 것이 확인된 것이지 버그가 아니므로 패치하지 않음.

## v0.1.13 (2026-09-21)

- **[정책 반영] AS7341 적분시간 파라미터화를 레퍼런스와 동일한 방식으로 정정** — 논문
  (Ban et al., §11 항목5) 재현 가능한 장비를 먼저 만들고 그 위에서 성능을 올리기로
  한 사용자 결정에 따라, "레퍼런스와 동일 세팅"을 우선 적용. 기존에는 ASTEP=999
  고정 + ATIME 가변(물리적으로 등가이나 레퍼런스와 반대 방향)이었던 것을,
  `_ref_fnirs_example`(src/as7341.c `as7341_set_integration_20ms_device()`)과 동일하게
  **ATIME=99(`AS7341_ATIME_FIXED`) 고정 + ASTEP 가변**으로 변경(`m_i2c_as7341.c/.h`).
  `m_i2c_as7341_set_integration_time()`의 파라미터 의미가 "ATIME 레지스터값"에서
  "integration_20ms_units"(BLE AS7341_CONFIG 필드와 동일 단위, 레퍼런스와 동일 semantics)로
  바뀌었다 — `m_i2c.c apply_pending_ble_config()`의 별도 단위 변환 코드
  (`integration_units_to_atime()`)도 더 이상 필요 없어 삭제.
- **[기본값 변경, 실기 재검증 필요]** 부팅 시 기본 적분시간을 기존 83.4ms(ATIME=29/
  ASTEP=999)에서 레퍼런스 기본값과 동일한 **약 19.7ms**(units=1, ATIME=99/ASTEP=70,
  `_ref_fnirs_example`의 `AS7341_DEFAULT_INTEGRATION_20MS=1`과 동일)로 변경. gain=9(256x)는
  그대로 유지(레퍼런스도 CFG1 리셋 기본값이 동일하게 9). 83.4ms는 2026-09-15 실기에서
  "6채널 모두 안정적인 비영 값" 확인된 값이었으므로, 19.7ms로 짧아진 이번 기본값에서도
  신호가 노이즈 수준으로 떨어지지 않는지 **다음 실기 검증에서 재확인 필요**(TODO
  open-item, architecture.md §11 항목5의 gain/ATIME 실측 캘리브레이션과 함께 처리 예정).
- **빌드 검증 완료** — `west build -b nrf52dk/nrf52832 Application -p always`(VERSION 파일
  변경 반영을 위해 pristine 필요, v0.0.16과 동일 이슈) clean build 성공. FLASH 156796B/
  216752B(72.34%), RAM 57584B/64KB(87.87%, 기존 v0.0.15 대비 RAM 사용량이 늘어난 것은
  이번 변경이 아니라 그 사이 추가된 BLE/OTA 버퍼 설정 누적분). `dfu_application.zip`
  manifest 확인 결과 `version_MCUBOOT: "0.1.13+0"` 정상 반영. **하드웨어 검증은 미실시**
  (이 세션 환경엔 실물 보드/J-Link 연결이 없음) — `west flash`로 실기 플래시 후 raw 데이터
  질적 변화(19.7ms 적분시간에서도 노이즈 수준으로 안 떨어지는지) 확인 필요.

## v0.1.14 (2026-09-21)

- **[버그 수정, 고위험(Watchdog/Reset)] v0.1.13 플래시 후 보드가 부팅 중 재부팅되는 현상
  — 원인: 버전 점멸 LED 블로킹 구간에서 watchdog stale 오탐**. 사용자가 "이번 패치 이후
  리셋되는 것 같다"고 보고, 버전 점멸 LED를 원인으로 의심해 조사 요청.
  - `m_ctrl.c watchdog_init()`은 CTRL 태스크 시작 즉시(부팅 초반) watchdog을 무장한다
    (`WATCHDOG_TIMEOUT_MS=4000ms`). `feed_watchdog_if_alive()`는 I2C/BLE 두 소스 모두
    최근 `WATCHDOG_ALIVE_STALE_MS(1000ms)` 이내에 `m_ctrl_notify_alive()`를 호출했어야 feed.
  - `m_i2c.c i2c_init()`의 버전 점멸 루프(`FW_VERSION_BOOT_BLINK_MS=150ms` × 2 ×
    `(FW_VERSION_PATCH+1)`회)는 **블로킹**인데, I2C 태스크의 첫 `m_ctrl_notify_alive
    (CTRL_ALIVE_I2C)` 호출은 이 루프가 끝나고 RTC tick 메인루프에 들어가야만 발생한다.
    v0.1.13(PATCH=13) 기준 점멸 시간이 **14×300ms=4200ms**로 watchdog 타임아웃(4000ms)을
    이미 초과 — `FW_VERSION_PATCH`가 매 패치(CHANGELOG 규칙상 무조건 +1)마다 늘어나므로
    이전부터 존재하던 시한폭탄이 이번 버전에서 실제로 터진 것(v0.1.11=3600ms, v0.1.12=
    3900ms로 이미 여유가 거의 없었음).
  - 추가로 `m_ble.c ble_init()`이 `k_sem_take(&sem_i2c_init_done, K_FOREVER)`로 I2C 초기화
    완료를 기다리는 동안 BLE의 첫 alive 신호도 함께 지연돼, 부팅 초반 **두 소스가 동시에
    stale**해지는 구조였다(둘 다 필요조건이라 하나만 고쳐도 근본 해결이 안 됨).
  - **수정**: ①`m_i2c.c` 점멸 루프 매 반복 + AS7341 init 완료 직후에 `m_ctrl_notify_alive
    (CTRL_ALIVE_I2C)` 추가. ②`m_ble.c`의 `K_FOREVER` 대기를 `K_MSEC(200)` 타임아웃 반복
    폴링으로 변경, 대기 중에도 매번 `m_ctrl_notify_alive(CTRL_ALIVE_BLE)` 호출. 둘 다 정상
    대기/초기화 구간에서만 alive를 더 자주 보고하는 추가적 변경이라, 실제 I2C 버스 행/BLE
    스택 초기화 실패 등 **진짜 hang은 여전히 감지**한다(watchdog 보호 기능 약화 없음).
  - **빌드 검증 완료**: clean pristine rebuild 성공, `version_MCUBOOT: "0.1.14+0"` 확인.
    **하드웨어 검증 필요**(실기에서 부팅 후 재부팅 없이 정상 진행되는지 RTT로 확인) — 이
    세션 환경에는 실물 보드 없음.
  - TODO(open-item): `FW_VERSION_PATCH`가 계속 늘어나면 버전 점멸 시간 자체는 계속
    길어진다(현재 v0.1.14 기준 15×300ms=4500ms) — watchdog stale 문제는 해소했지만, 부팅
    체감 시간이 계속 늘어나는 것은 별개 이슈. 점멸 횟수를 `FW_VERSION_PATCH % N` 등으로
    캡하는 것을 다음에 검토 필요(architecture.md §11 후보 등록 권장).

## v0.1.15 (2026-09-21, **롤백됨 — 아래 v0.1.16 참고**)

- **[신규, 고위험(보안), 같은 날 v0.1.16에서 롤백됨] BLE 페어링/본딩 활성화 — architecture.md §11 항목6-[5]에 설계만
  준비돼 있던 것을 사용자 결정으로 활성화(펌웨어 먼저 구현 후 앱에 적용 예정)**.
  - `prj.conf`: `CONFIG_BT_SMP=y`/`CONFIG_BT_BONDABLE=y` 활성화. 재부팅 후에도 "자동
    재연결"이 유지되도록 본딩 정보(LTK)를 flash(NVS)에 영구 저장하는
    `CONFIG_BT_SETTINGS`/`CONFIG_SETTINGS`/`CONFIG_NVS`/`CONFIG_FLASH_MAP` 계열 신규 추가.
  - `m_ble_gatt.c`: `CONFIG` characteristic read/write, `DATA0`/`DATA1`의 CCC descriptor
    permission을 `BT_GATT_PERM_*_ENCRYPT`로 변경 — 본딩(암호화)되지 않은 연결은 CONFIG
    접근/DATA0·DATA1 구독이 거부된다.
  - `m_ble.c`: `on_connected()`에서 연결 즉시 `bt_conn_set_security(conn, BT_SECURITY_L2)`
    호출로 본딩을 선제적으로 트리거(앱이 characteristic에 접근할 때까지 기다리지 않음).
    `bt_conn_auth_cb`/`bt_conn_auth_info_cb` 등록, `security_changed` 콜백 추가(결과 로깅).
    `bt_enable()` 이후 `settings_load()` 호출로 이전 세션의 본딩 정보 복원.
  - **IO capability 결정**: 이 보드는 디스플레이/입력 수단이 전혀 없어(pinmap.md)
    passkey/confirm 콜백을 등록하지 않았다 — Zephyr가 자동으로 NoInputNoOutput으로
    취급해 **Just Works 페어링만 수행**(MITM 인증 불가, 하드웨어 구조상 불가피).
  - **부수 효과**: `CONFIG_BT_SMP=y`로 `CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU`의
    `MCUMGR_TRANSPORT_BT_PERM`도 암호화 필요 옵션으로 자동 전환됨 — OTA(SMP)도 이제
    본딩된 연결에서만 가능해진다(기존 OTA 검증, CHANGELOG v0.0.17 이하는 비암호화
    기준이었으므로 재검증 필요).
  - **빌드 검증 완료**: clean pristine rebuild(`west build -b nrf52dk/nrf52832 Application
    -p always`) 성공, `dfu_application.zip` manifest `version_MCUBOOT: "0.1.15+0"` 확인.
  - **미검증 상태**: 테스트 APK가 BLE 페어링 UI/본딩을 지원하는지 이 세션에서 확인하지
    못했다 — 지원하지 않으면 기존 앱 연동(v0.0.12 검증분)이 CONFIG/DATA0/DATA1에서 깨질
    수 있다. **하드웨어 검증(실제 페어링 시퀀스, 재부팅 후 재페어링 없이 재연결)은 이
    세션에 실물 보드가 없어 미실시**.
  - TODO(open-item): 앱 쪽 본딩 지원 확인 및 대응 완료 후, OTA(SMP) 재검증 필요.

## v0.1.16 (2026-09-21)

- **[롤백] v0.1.15 BLE 페어링/본딩 전면 롤백 — 사용자 결정**: "본딩 로직은 롤백해줘,
  아직은 구현할 필요가 없을 것 같다 — 앱과 연동이 끊어진 후 디바이스가 다시 advertising
  으로 전환되서 재연결만 되면 충분하다"는 판단. 그 재연결 동작은 v0.1.15 이전부터
  `m_ble.c on_disconnected()`가 이미 담당하고 있어(연결 해제 시 advertising 재시작,
  2026-09-16 검증 완료) 별도 구현이 필요 없었다.
  - `prj.conf`: `CONFIG_BT_SMP`/`BONDABLE`/`SETTINGS`/`NVS`/`FLASH_MAP` 계열 전부 원복,
    항목6-[5] 주석을 "설계만 준비, 비활성" 원래 상태로 되돌리되 2026-09-21 활성화→롤백
    이력만 짧게 추가.
  - `m_ble_gatt.c`: CONFIG characteristic, DATA0/DATA1 CCC permission을
    `BT_GATT_PERM_READ`/`WRITE`(암호화 불필요)로 원복.
  - `m_ble.c`: `bt_conn_set_security()`/`bt_conn_auth_cb`/`bt_conn_auth_info_cb`/
    `security_changed`/`settings_load()` 등 v0.1.15에서 추가한 코드 전부 제거.
  - v0.1.15는 이 세션에서 커밋되지 않았고 실물 보드에 플래시된 적도 없어, 기존 앱 연동
    (v0.0.12)에 실질적 영향은 없었음.
  - architecture.md §11 항목6-[5]는 다시 "보류" 상태로 복귀.

## v0.1.17 (2026-09-21)

- **[버그 수정] BLE 연결 끊김 시 ring buffer 용량과 grace period(§11 항목6-[3]) 불일치로
  인한 실제 데이터 유실 — architecture.md §2.5(`RING_BUFFER_CAPACITY`)와 §11 항목6-[3]
  (`BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS`)가 서로 다른 시점에 독립적으로 정해지면서
  생긴 불일치를 사용자 요청("연결 끊겼을 때 센싱 데이터 ring buffer 저장 로직 구현")으로
  재검토하다 발견**.
  - "끊겨도 계속 측정+버퍼링" 로직 자체(`m_i2c.c`/`m_ble.c`)는 v0.0.14부터 이미 구현돼
    있었으나, `RING_BUFFER_CAPACITY`(150샘플=15초)가 `BLE_DISCONNECT_STANDBY_TIMEOUT_
    TICKS`(300 tick=30초)보다 작아서, 30초 끊김이 나면 15초 지점부터 이전 데이터가
    overflow로 실시간 덮어써져 유실됐다(`m_i2c_ring_buffer_push()` 특성상 silent는
    아니고 dropped_count는 증가하지만, 실제로는 유실).
  - `config_app.h`: `RING_BUFFER_CAPACITY`를 `BLE_DISCONNECT_STANDBY_TIMEOUT_TICKS`를
    직접 참조하도록 묶어 앞으로 두 값이 다시 어긋나지 않게 함.
  - **1차 시도(300 tick=30초 그대로 유지) 되돌림**: RAM 예산 검토 결과 Application 이미지
    RAM 사용률이 87.8%(150샘플, 기존)→96.07%(300샘플)까지 올라가는 것을 빌드로 실측,
    스택 오버플로우 등 위험 대비 여유(2.5KB)가 부족하다고 판단해 사용자에게 확인 요청.
  - **사용자 결정: "grace period를 버퍼가 감당 가능한 수준으로 단축"** — `BLE_DISCONNECT_
    STANDBY_TIMEOUT_TICKS`를 300→**220 tick(22초)**으로 하향. 결과 RAM 사용률 91.67%
    (60,080/65,536B)로 안전 마진 확보. 22초까지는 데이터 유실 0% 보장(그 이상 끊기면
    STANDBY 진입으로 측정 자체가 멈춰서 추가 유실도 없음).
  - **빌드 검증 완료**: clean pristine rebuild, `dfu_application.zip` manifest
    `version_MCUBOOT: "0.1.17+0"` 확인. **하드웨어 검증은 미실시**(실물 보드 없음) —
    실제로 22초 끊김/재연결 시나리오에서 데이터 gap이 없는지, 22초 초과 시 STANDBY로
    정상 전환되는지 실기 확인 필요.
  - TODO(open-item): 30초 grace period라는 원래 §11 항목6-[3] 값 자체가 정책값이
    아니라 초기값이라고 명시돼 있었음 — 이번 22초 변경도 마찬가지로 실측 후 조정
    가능한 값. RAM 여유를 늘리려면(예: 배터리/온도 센서 등 향후 기능 추가 시) 다시
    검토 필요.

## v0.1.18 (2026-09-21)

- **[프로토콜 변경, v2] DATA0/DATA1 notify 프레임에 timestamp_us/seq_num 추가 (8→16바이트)**
  — 사용자 요청, `NCS_TedreamS2`(`Application/src/a/m_a_ble.c`) 분석 반영. S2는 센서
  페이로드를 `[timestamp 4B LE][센서 데이터]` 순서로 구성하는 관례가 있어(architecture.md
  §2.4 "32bit us 타임스탬프" 정책과 일치) 이를 그대로 계승, S2의 SALT/VER 핸드셰이크·
  AES-CCM 암호화는 이 프로젝트 범위 밖이라 가져오지 않음(agents.md §4).
  - 새 프레임(`m_ble_gatt.h`/`m_ble_proto.h`/`m_ble_proto.c`): offset 0-3 timestamp_us
    (u32 LE), 4-7 seq_num(u32 LE), 8-9 Red630, 10-11 Red680, 12-13 NIR, 14-15 LED
    index(전부 u16 LE).
  - `BLE_PROTOCOL_VERSION` 1→2(`AS7341_VERSION` characteristic 0x1528로 조회 가능).
  - 앱 쪽도 8→16바이트 프레임 파싱으로 업데이트 필요(구버전 앱은 이 프레임을 더 이상
    올바르게 파싱 못 함 — 사용자가 앱을 함께 관리하므로 이번 범위에 포함해 진행).
  - 기존 `AS7341_SEQ`(0x1529) notify characteristic은 제거하지 않고 유지(하위 호환).
  - 기본 ATT MTU(23바이트, payload 20바이트) 안에 16바이트 프레임이 여전히 들어가므로
    별도 MTU 협상 없이도 동작.

- **[버그 수정, 고위험(안전상태)] BLE notify 혼잡(-ENOMEM) 시 안전상태 영구 래치되는
  회귀 방지** — v0.1.17에서 발견된 위험(재연결 후 최대 220샘플 backlog를 텀 없이 몰아
  notify하면 ATT 송신 버퍼 풀 고갈로 `bt_gatt_notify()`가 `-ENOMEM`을 반환할 수 있는데,
  기존 코드는 이를 `MODULE_ERR_BLE_TX_FAILED`로 오분류해 `m_ctrl.c`가
  `CTRL_STATE_DEGRADED`로 래치 — IEC 60601 단일고장 철학에 따라 **재부팅 전까지 자동
  복구 안 됨**, 즉 정상적인 backlog flush 상황에서 측정이 영구 정지될 수 있었음)를
  해소.
  - `m_ble.c`: `notify_sample()`/`notify_seq()`가 `-ENOMEM`을 `-ENOTCONN`/`-EINVAL`과
    같은 "무해한 전달 실패"로 분류(architecture.md §11 항목6-[4] 원칙과 일관) —
    `MODULE_ERR_BLE_TX_FAILED` 미보고, 안전상태 래치 안 됨.
  - 추가로 congestion(`-ENOMEM`) 감지 시 다음 pop 전 `BLE_NOTIFY_CONGESTION_BACKOFF_MS`
    (20ms) 대기를 넣어 컨트롤러가 송신 큐를 비울 시간을 확보 — 텀 없는 재시도로 혼잡이
    반복되는 것을 완화.
  - **빌드 검증 완료**: clean pristine rebuild, `dfu_application.zip` manifest
    `version_MCUBOOT: "0.1.18+0"` 확인. **하드웨어 검증 미실시** — 실제로 220샘플
    backlog flush 시 `-ENOMEM`이 재현되는지, congestion backoff로 안전상태 래치 없이
    정상 flush되는지, 새 16바이트 프레임을 앱이 올바르게 파싱하는지 모두 실기 확인 필요.

## v0.1.19 (2026-09-21)

- **[전력 최적화] DATA0/DATA1 배칭 도입 + LED index 필드 제거 + connection interval/
  peripheral latency 조정 요청** — 사용자 지적: "지금 Data에 LED Index가 필요한가?
  10Hz마다 매번 notify하면 전력 소모가 클 것 같다, 프레임을 키워서 데이터를 묶어
  보내자." 사용자와 두 가지 방향(범위: 배칭+connection interval 조정 함께 진행,
  배치 크기: 협상된 MTU에서 자동 산출) 확인 후 진행.
  - **LED index 제거**: `m_ble.c BLE_SAMPLE_LED_INDEX_FIXED`가 순차 LED 스트로빙
    미구현으로 매 샘플 고정값 0만 나가던 죽은 필드였음 — 확인 후 제거.
  - **DATA0/DATA1 v3 배칭 프레임**(`m_ble_proto.h`/`.c`, `BLE_PROTOCOL_VERSION` 2→3):
    `[sample_count(1B)][record×N]`, record=timestamp_us(4B)+seq_num(4B)+Red630/
    Red680/NIR(u16×3)=14바이트. N은 MTU 247(payload 244) 기준 상한 17, 실제 배치
    크기는 `m_ble.c` MTU exchange 콜백이 매 연결마다 계산(협상 실패 시 N=1로 자동
    축소, 별도 레거시 포맷 없이 동일 파서로 처리).
  - **MTU 협상 + LE Data Length Update 우리(peripheral)가 선제 요청**
    (`bt_gatt_exchange_mtu()`/`bt_conn_le_data_len_update()`, `prj.conf`
    `CONFIG_BT_GATT_CLIENT`/`CONFIG_BT_USER_DATA_LEN_UPDATE` 추가) — 컨트롤러는 이미
    251byte까지 지원하도록 기본 설정돼 있었음(`CONFIG_BT_CTLR_DATA_LENGTH_MAX=251` 등,
    빌드 .config로 확인) — 앱/중앙기기가 먼저 요청 안 해도 동작하도록 함.
  - **connection interval/peripheral latency 조정 요청**(`bt_conn_le_param_update()`):
    30ms interval, latency 4(유휴 시 연결 이벤트 4/5 스킵) — architecture.md §2.4가
    원래 열어둔 "S2 선례 범위(7.5~30ms), 실측 후 확정"의 초기값. 배칭만 하고 이 조정을
    안 하면 라디오가 여전히 자주 깨는데 보낼 데이터가 없는 상황이 되어 전력 절감
    효과가 제한적이라는 점을 사용자와 확인 후 함께 적용.
  - **AS7341_SEQ(0x1529) notify 중단**: 배칭 프레임 레코드 자체에 seq_num이 포함돼
    "DATA0/DATA1과 같은 tick에 SEQ도 함께 notify" 개념이 배칭과 안 맞아 호출 제거.
    GATT characteristic 선언은 하위 호환 위해 유지(구독해도 비용 없음, 그냥 안 옴).
  - **재연결 안전성 설계**: 배치 버퍼(`s_batch`, 최대 17샘플)에 샘플이 오래 머무르면
    그 사이 연결이 끊길 때 ring buffer 기반 재연결 안전장치(§11 항목6-[3]) 밖에서
    유실될 위험이 있다 — "배치가 다 찼거나 flush timeout(2초)일 때만 필요한 만큼
    한 번에 pop 후 즉시 notify"하는 구조로 유실 위험 구간을 pop+notify 처리 시간
    (수 ms 이하)으로 최소화했다. 이를 위해 `m_i2c_ring_buffer_count()`(pop 없이 개수만
    확인) 신규 추가.
  - **빌드 검증 완료**: clean pristine rebuild, `version_MCUBOOT: "0.1.19+0"`,
    RAM 92.50%(60,624B, +544B), FLASH 74.10%(+3.7KB, GATT_CLIENT/DLE 코드).
    **하드웨어 검증 미실시** — 실제 배치 크기가 기대대로 산출되는지, connection
    param 조정이 실제로 받아들여지는지(중앙기기가 거부/재협상 가능), 앱이 새
    배칭 프레임을 올바르게 파싱하는지, 전력 절감이 실측되는지(AT-06) 전부 확인 필요.

## v0.1.20 (2026-09-22)

- **[전력 최적화] "한 프레임에 몇 개를 담을지"(MTU 결정)와 "얼마나 자주 무선을 깨워
  보낼지"(전력 목표)를 분리 — 사용자 질문("최소 몇 묶음을 전달하는 게 좋을까? 센싱
  주기에 맞추는 게 좋을까 저전력을 위해 BLE Task wake 주기에 맞추는 게 좋을까?")에
  답하며 발견한 v0.1.19의 빈틈 수정**.
  - v0.1.19까지는 "배치가 다 찼거나 2초 지나면 flush"만 있어서, MTU 협상이 잘 안 돼
    프레임당 1개만 담기는 상황(`s_batch_capacity=1`)에서는 사실상 매 tick(100ms)마다
    즉시 전송돼 배칭 효과가 없었다.
  - **사용자 결정: 무선을 깨우는 최소 간격을 센싱 주기가 아니라 전력 목표 기준으로
    1초로 고정**(`BLE_BATCH_FLUSH_INTERVAL_MS=1000`, `m_ble.c`) — cycle/active
    게이팅 설정이 바뀌어도 무선 송신 빈도(배터리 소모의 핵심 변수)는 항상 일정하게
    유지된다.
  - `m_ble_task_entry()` 루프 재설계: 1초 주기가 될 때까지는 pop하지 않고 ring
    buffer에 쌓아두기만 하다가, 주기가 되면 쌓인 것 전부를 그 자리에서 한 번에
    비운다 — 한 프레임(MTU 기준 최대 17개)보다 많이 쌓였으면 여러 프레임으로 나눠
    같은 "깨어난 시점"에 몰아서 보낸다(무선을 깨우는 횟수 자체는 1회로 유지).
  - 기존 congestion backoff(`-ENOMEM` 처리, v0.1.18)는 이 다중 프레임 전송 루프
    안에서도 그대로 적용.
  - **빌드 검증 완료**: clean pristine rebuild, `version_MCUBOOT: "0.1.20+0"`,
    RAM/FLASH 사용률 변화 없음(로직 재구성, 신규 저장공간 없음). **하드웨어 검증
    미실시** — MTU가 작게 협상되는 조건(구형 폰 등)에서 실제로 1초 간격으로만
    무선이 깨는지, 정상 MTU 조건에서는 기존과 동일하게 동작하는지 확인 필요.
  - TODO(open-item): 1초는 실시간성/배터리 트레이드오프의 초기값 — AT-06 전류소비
    실측 후 조정 가능.

## v0.1.21 (2026-09-22)

- **[정정] `Application/VERSION`이 디스크에서 19로 되돌아가 있어 `config_app.h`의
  `FW_VERSION_PATCH`(20)와 어긋난 상태를 발견 — 이번 패치로 두 값을 21로 다시
  일치시킴.** (memory: OTA 버전 파일 동기화 필요 — 동일 사안 재발)
- **[버그 수정] 부팅 버전 점멸 횟수를 `(FW_VERSION_PATCH+1)`회 → 고정 3회로 변경**
  (사용자 요청). 기존 방식은 패치 버전이 오를수록 점멸 시간(=부팅 블로킹 구간)이
  계속 길어져 watchdog 타이밍과 충돌하는 고위험 버그로 실제 이어졌던 구조(v0.1.13→
  v0.1.14)라, 고정 횟수로 바꿔 이 클래스의 버그 자체를 근본적으로 제거함. OTA 적용
  여부 확인은 이제 점멸 횟수 대신 BLE `AS7341_VERSION` characteristic(0x1528)으로
  `fw_version`을 읽는 방식으로 대체(앱이 이미 지원) — **점멸만으로 버전 구분은
  더 이상 안 됨, 육안 확인은 "부팅이 여기까지 왔다"는 생존 신호 용도로만 남음.**
  `config_app.h`(`FW_VERSION_BOOT_BLINK_COUNT=3`), `m_i2c.c` 루프 수정.
- **[신규, 하드웨어 진단용] `main()`에 LED1(640nm)/LED2(680nm) 강제 점등 추가** —
  사용자 보고: "SW1을 눌러도 한 번에 켜지지 않는 보드가 발생, 전원이 안 들어가는
  건지 초기화 단계 에러인지 구분이 안 됨." `main.c`는 커널이 스케줄링을 시작하면
  어떤 태스크(m_i2c/m_ble/m_ctrl) init 성공 여부와 무관하게 반드시 도달하므로,
  여기서 직접 `m_i2c_led_init()`+`m_i2c_led_set_duty()`를 호출해 LED를 켜면
  "적어도 전원+부팅까지는 성공"을 무조건 확인할 수 있다(태스크 init이 멈추면
  기존 LED 시퀀스는 아예 안 나와서 두 경우를 구분 못 했음).
  - **아키텍처 원칙의 의도적 예외**: `agents.md`/`architecture.md` §6("main()은
    태스크를 생성/초기화하지 않는다") 및 `m_i2c_led.h`("I2C Task 컨텍스트에서만
    호출")를 하드웨어 진단 목적으로 명시적으로 벗어난다. main() 시점엔 아직 어떤
    태스크도 AS7341 측정을 하지 않으므로 실측 정확도와는 충돌하지 않으며, m_i2c
    태스크가 정상 기동하면 자신의 LED 시퀀스로 자연히 덮어쓴다. `main.c`/
    `m_i2c_led.h`에 예외 사유 명시.
- **[정책] BLE 디바이스 이름을 `nRF_fNIRS_Sys_v2`(고정) → `TedNeuro_v0.1.21`(버전
  포함)로 변경**(사용자 요청) — BLE 스캔/연결 시 앱이 디바이스 이름만 보고도
  펌웨어 버전을 바로 구분할 수 있게 함. `prj.conf CONFIG_BT_DEVICE_NAME`은 Kconfig
  문자열이라 C 매크로 자동 대입이 안 돼, **앞으로 매 패치 버전 상승마다 VERSION
  파일/`config_app.h`와 함께 수동으로 갱신하는 정책**으로 확정(주석에 명시).
- **빌드 검증 완료**: clean pristine rebuild, `version_MCUBOOT: "0.1.21+0"`,
  `.config`에서 `CONFIG_BT_DEVICE_NAME="TedNeuro_v0.1.21"` 반영 확인. RAM
  92.50%(변화 없음), FLASH +204B.
- **2026-09-22 실기 검증 완료(BLE 데이터 수집 경로)**: v0.1.21을 실물 보드에 플래시,
  앱과 연동해 85.5초간 raw 데이터 수집(`2026-09-22_10-36-38_fNIRS_RAW_DATA.csv`,
  1730 레코드) — seq_num 865개 전부 연속(gap 0), device_timestamp_us 간격 865개
  전부 정확히 100,000us(표준편차 0), v0.1.20 배칭 설계(10Hz×1초)와 실측 배치 크기
  일치. v0.1.13에서 낮춘 적분시간(19.7ms)의 노이즈 우려도 해소(전 채널 포화/저신호
  없음). v0.1.13→v0.1.14 재발 방지용 부팅 점멸 고정 3회 변경도 이 세션에서 정상
  부팅 확인. 상세는 architecture.md §2.4/§11 항목5 참고. **여전히 미검증**: 연결
  끊김이 없어 재연결/22초 backlog/`-ENOMEM` 경로, LED1/LED2 강제 점등의 실제 진단
  효용, 새 디바이스 이름 스캔 노출.

## v0.1.22 (2026-09-22)

- **[버그 수정] AS7341 측정을 LED on 구간에 맞춰 매 샘플마다 명시적으로 재트리거**
  (`m_i2c_as7341.c`) — 오늘 레퍼런스 raw 데이터와 비교 분석하다가 발견: LED 듀티를
  50%→75~100%까지 크게 올려도 raw 채널 비율이 거의 안 움직였고, 완전 암실에서
  LED만 켜고 측정했더니 대부분 1~2에 불과하고 **정확히 53샘플(sensor1)/139샘플
  (sensor0) 주기로만 큰 값이 튀는 beat(맥놀이) 패턴**이 실측으로 확인됨
  (`2026-09-22_15-19-34_fNIRS_RAW_DATA.csv`, `docs/test/`).
  - **원인**: v0.1.12에서 SP_EN(스펙트럴 엔진)을 init 1회만 켜고 이후 절대
    재기록하지 않는 free-running 방식으로 바꿨다(`_ref_fnirs_example` 레퍼런스와
    동일 방식이라는 근거). 그런데 그 레퍼런스는 **LED가 상시 켜져 있는 구조**라
    free-running이어도 항상 LED와 적분 구간이 겹치지만, 우리 프로젝트는
    `m_i2c.c acquire_one_sample()`이 **매 샘플마다 LED를 켰다/끄는 구조**
    (architecture.md §5 "연속점등 금지")다. 두 독립적인 주기(LED on/off tick vs
    센서의 free-running 적분 주기)가 서로 비동기로 돌면서 위상이 주기적으로만
    맞는 beat 패턴이 발생 — 위상이 맞을 때만 실제 LED 신호가 잡히고 나머지는
    거의 0에 가까운 값(ambient/누설광 수준)이 찍히고 있었다.
  - **수정**: `m_i2c_as7341_read_raw()`가 호출될 때마다 ENABLE 레지스터를
    `PON`만 → `PON|SP_EN`으로 재기록해 새 적분 사이클을 명시적으로 시작한다
    (datasheet §10.2, SP_EN 재기록 시 스펙트럴 엔진 재시작). `m_i2c.c`가 LED를
    켠 "다음"에 이 함수를 호출하므로, 이렇게 하면 읽는 적분 구간이 항상 LED on
    구간 시작 이후로 보장된다. `m_i2c_as7341_init()`은 이제 SP_EN을 켜지 않고
    PON만 걸어둔다(트리거는 매 read마다 read_raw()가 담당).
  - v0.1.12의 "매 tick ENABLE 재기록은 불필요한 중복이라 버그"라는 판단은
    레퍼런스(상시점등)에는 맞았지만 우리 아키텍처(LED 온/오프 duty)에는 맞지
    않았던 것으로 정정 — 결과적으로 v0.0.4~v0.1.11 시절의 "매 read마다 재기록"
    방식이 우리 구조에는 옳았고, v0.1.12~v0.1.21 구간의 free-running이 이번에
    발견된 beat 패턴 버그를 만들고 있었다(이 구간 동안의 raw 데이터 절대값은
    LED-동기화가 깨진 상태에서 수집된 것으로 재해석 필요, architecture.md §11
    항목5 참고).
  - **빌드 검증 완료**(`west build -b nrf52dk/nrf52832 Application -p always`,
    `APPVERSION=0.1.22`, 에러 없음). **하드웨어 검증 미실시** — 다음 세션에서
    실물 보드로 (1) 완전 암실 beat 패턴이 사라지는지, (2) 듀티 변경이 이제
    채널 비율에 정상적으로 반영되는지 재검증 필요.
- BLE 디바이스 이름 `TedNeuro_v0.1.22`로 갱신(정책, VERSION/config_app.h와 동기화).
- **2026-09-22 실기 검증 완료(beat 패턴 해소)**: 완전 암실에서 재측정 결과 0인
  행/스파이크 없이 전 채널 변동률 1% 미만으로 안정 — beat 패턴 해소 확인. 실제
  부착 재측정(2회)에서도 채널비율이 레퍼런스 방향으로 재현성 있게 개선(d1
  red680/red630 0.71→1.5대, d2 1.39→2.5~2.8대). **새 회귀 발견**: 앱이 device
  timestamp를 CSV에 직접 기록하도록 개선한 뒤 보니, 샘플 간격이 설정된 Cycle
  300ms가 아니라 **일관되게 400ms**로 나옴(v0.1.23에서 원인 규명 및 수정, 아래).

## v0.1.23 (2026-09-22)

- **[버그 수정] AS7341 NIR1/NIR2 트리거를 먼저 둘 다 걸고, 그 다음에 순서대로
  읽도록 변경** (`m_i2c_as7341.c`, `m_i2c_as7341.h`, `m_i2c.c`) — v0.1.22에서
  측정 주기가 의도한 300ms가 아니라 400ms로 밀리는 회귀가 실기로 확인됨(device
  timestamp 기준 105개 샘플 중 104개가 정확히 0.4s 간격).
  - **원인**: v0.1.22의 `m_i2c_as7341_read_raw()`는 트리거(SP_EN 재기록)+적분
    대기+읽기를 한 함수에서 다 처리했다. `m_i2c.c acquire_one_sample()`이 NIR1,
    NIR2를 **순차로** 이 함수를 호출하다 보니, NIR1의 적분 대기시간(최대
    `AS7341_MEASURE_TIMEOUT_MS`)이 끝나야 NIR2 트리거가 걸리고, NIR2도 또
    자기 적분을 처음부터 기다려야 했다 — 두 센서의 적분 대기가 더해져 한 샘플을
    만드는 데 걸리는 시간이 늘어나 RTC tick 기준 300ms 주기를 못 맞추고 400ms로
    밀린 것.
  - **수정**: `m_i2c_as7341_read_raw()`에서 트리거 부분을 분리해
    `m_i2c_as7341_trigger_measurement()`로 뽑아냈다. `acquire_one_sample()`이
    이제 NIR1/NIR2를 **먼저 둘 다 트리거**(적분이 동시에 시작)한 뒤, **그 다음에
    순서대로 읽는다**(`m_i2c_as7341_read_raw()`, 이제 트리거 없이 AVALID 폴링+
    읽기만 함) — 두 센서의 적분 대기시간이 겹쳐 진행되므로 추가 지연이 대략
    절반으로 줄어든다. LED-적분 동기화(v0.1.22의 핵심 수정)는 그대로 유지됨
    (트리거는 여전히 LED를 켠 "다음"에 걸림).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `APPVERSION=0.1.23`, 에러 없음. **하드웨어 검증 미실시** — 실물 보드로 (1)
    샘플 간격이 300ms로 복귀하는지, (2) beat 패턴이 재발하지 않는지(트리거 시점이
    바뀌었으므로 회귀 가능성 재확인 필요) 검증해야 한다.
- BLE 디바이스 이름 `TedNeuro_v0.1.23`으로 갱신.
- **2026-09-23 실기 검증 완료**: 약 78분(4676초) 연속 수집 세션에서 device timestamp
  기준 15,586/15,588 샘플이 정확히 300ms 간격(표준편차 0)으로 확인 — beat 패턴 재발
  없음. 세션 중간(약 71.5분 지점) 원인 불명의 기기 재부팅 1회 발생했으나 재연결이
  약 1초 만에 이뤄져 데이터 끊김/이상값 없이 이어짐(재부팅 원인 별도 확인 필요).

## 2026-09-23 — 문서 정정 (코드 동작 변경 없음)

- `m_ble_proto.h`의 "cycle period/active window가 CONFIG에 저장만 되고 실제 RTC
  샘플링 주기에는 반영되지 않는다"는 TODO(open-item) 주석이 **오래돼 사실과 다름**을
  발견해 정정. 실제로는 v0.1.5(2026-09-18)에서 이미 tick 기반 게이팅(`m_i2c.c
  is_gate_active_tick()`)으로 구현됐고 v0.1.6(2026-09-18)에서 실기 검증까지 끝난
  사항이었다(당시 CHANGELOG에 기록돼 있었으나 이 헤더 주석만 갱신이 안 됨).
  Integration time 반영도 코드 확인 + 2026-09-22/23 실기 테스트로 재확인.
  **실기 최종 재검증(2026-09-23)**: 앱에서 LED 듀티/Integration time을 변경하며
  실시간 그래프로 확인 — 설정 적용 시점에 정확히 계단식으로 반응, Integration
  5→1(20ms 단위, 100ms→19.7ms, 5배 차이) 변경 시 raw count가 약 5.2배 감소해
  데이터시트 공식(tint 및 full-scale이 (ATIME+1)×(ASTEP+1)에 비례)과도 일치함을
  확인. **여전히 미반영인 항목은 location interval(offset 1)뿐** — CONFIG
  read-back만 되고 실제 동작에는 반영 안 됨(그대로 TODO 유지).

## v0.1.24 (2026-09-28)

- **[신규 기능] Ambient light 제거(dark-frame subtraction) 도입** — 9/24~27 3박 수면
  데이터 분석 중 "레퍼런스는 전체 행의 50%가 다크 프레임(LED off, ambient-subtracted)인데
  우리 raw는 다크 프레임이 없어 raw 정의 자체가 다르다"던 architecture.md §11 항목5의
  미해결 항목에 대응.
  - `m_i2c.c`: `acquire_one_sample()`을 `acquire_common(sample, dark)`으로 리팩터링하고
    `acquire_one_dark_sample()`을 추가. lit `DARK_FRAME_LIT_INTERVAL`(10)개마다 다크
    프레임 1개를 추가로 ring buffer에 push한다(사용자 결정 — 다크는 주변광/dark
    current라 생리신호처럼 빠르게 안 변해서 lit과 1:1일 필요가 없고, 1:10이면 데이터량/
    전력 증가를 +10%로 제한하면서도 오프셋 추적엔 충분). 다크 프레임은 LED를 전부 끈
    채로 lit과 **동일한 gain/ATIME/ASTEP**으로 측정(절대 바꾸지 않음) — AS7341은
    적분시간에 선형 비례하는 누적(합)연산이라(9/23 실측: 적분 5배→count 5.2배, 곱연산
    아님) 앱이 스케일링 없이 `lit_raw - dark_raw`만 하면 순수 LED 신호를 얻을 수 있다.
    다크 프레임에는 `check_sensor_sanity()`(포화/저신호 판정)를 적용하지 않는다 — 다크는
    원래 low signal이 정상이라 매번 오탐되는 걸 방지.
  - `nirs_sample_t`(`nirs_sample.h`)에 `bool is_dark` 필드 추가.
  - **BLE 프로토콜 v3→v4**(`BLE_PROTOCOL_VERSION`, `m_ble_proto.h`): 배칭 레코드
    바이트 레이아웃/길이(14바이트)는 그대로 두고, 기존 `seq_num`(offset+4-7) 필드의
    **최상위 1비트(bit31)를 다크 프레임 플래그로 재사용**(새 필드 추가/프레임 크기 변경
    없음) — 앱은 이 프로토콜 버전을 읽고 파서를 갱신해야 한다(상세 스펙은
    `app_handoff_dark_frame_context.md` 참고).
  - **RAM 영향 없음**: `RING_BUFFER_CAPACITY`는 정적 배열 크기라 push 빈도가 +10%
    늘어도 재조정 불필요(기존 용량 여유로 흡수).
  - **빌드 검증 완료**(`west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.24+0"`, 에러 없음, App Flash 74.29%/RAM 92.50% — v0.1.23
    대비 RAM 변화 없음, 구조체 패딩에 흡수됨). **하드웨어 검증 미실시** — 다음
    세션에서 (1) 완전 암실에서 다크 채널값 안정성, (2) 앱이 v4 파서로 lit/dark 분리
    후 레퍼런스 대비 채널비율 재비교 필요.

## v0.1.25 (2026-09-28)

- **[정정] 클럭 트리 재계산 검증 완료** — 트래커 "0. Baseline & Handover" §2번 항목
  (`2) 클럭 트리/타이머 프리스케일러 32MHz 기준 재계산 검증`, 그동안 70%에서 정체)
  마무리. LFCLK 소스(`CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL`)는 이미 기본값으로
  맞았으나, **정확도(ppm) 선언은 한 번도 검토된 적이 없어 Zephyr 기본값인
  50ppm(31~50ppm 구간)으로 방치**돼 있었다. 실제 실장 부품(`pinmap.md` §5, LFXO
  ECS Inc. ECS-.327-12.5-12-TR) 데이터시트 기준 정확도는 **±20ppm**으로 확인,
  `CONFIG_CLOCK_CONTROL_NRF_K32SRC_20PPM=y`로 정정(`prj.conf`). 이 값은 BLE
  컨트롤러가 연결 이벤트마다 클럭 drift에 대비해 RX 윈도우를 얼마나 넓힐지
  계산하는 데 쓰인다 — 지금까지는 실제보다 부정확한 크리스탈을 가정해 RX 윈도우를
  필요 이상으로 넓게 열어 라디오 on-time과 전력을 낭비하고 있었을 가능성(안전하지만
  비효율적인 방향의 오차). 8h 배터리 목표(§8)와 연결된 낮은 리스크 전력 최적화.
  - **부수 발견(문서 오기 추정)**: `pinmap.md`의 HFXO 부품번호 `ECS-240-8-37CKM`은
    ECS Inc. 명명 규칙상 24MHz 부품이다(32MHz는 `ECS-320-8-37CKM`) — nRF52832
    HFXO는 반드시 32MHz여야 하고 BLE이 지금까지 정상 동작해온 정황상 실물은 32MHz가
    맞을 가능성이 높다. 이전 LED3(950nm/980nm) 사례와 같은 유형의 **문서 오기로
    추정** — 실물 보드 각인 확인 필요(TODO(open-item), `pinmap.md` §5에 기록).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.25+0"`, `.config`에서 `CONFIG_CLOCK_CONTROL_NRF_ACCURACY=20`
    반영 확인, App Flash 74.29%/RAM 92.50%(변화 없음, 순수 설정값이라 코드 크기 영향
    없음). **하드웨어 검증 미실시** — BLE 실측 전류소비 변화(AT-06)는 별도 확인 필요
    (효과가 있더라도 매우 작을 것으로 예상, 리스크 낮은 정정 성격).

## v1.0.0 (2026-10-08, `dev/change_sampling_logic` → main 병합 — **v1.0 릴리스**)

- **[릴리스] v1.0.0 — 코드 변경은 버전 표기뿐이다(동작 코드는 v0.1.39와 동일).** `Application/VERSION`
  (MAJOR=1, MINOR=0, PATCHLEVEL=0), `config_app.h FW_VERSION_MAJOR/MINOR/PATCH`(1/0/0, 패킹 `0x1000`),
  `prj.conf CONFIG_BT_DEVICE_NAME="TedNeuro_v1.0.0"` 세 곳을 함께 올렸다. 이 세 곳이 어긋나면 OTA가
  버전 체크로 거부될 수 있다.
- **v1.0 확정 사양(2026-10-06~07, `v1.0_결정사항.md`, `v1.0_파라미터_확정.md`)**: LED 3파장 동시 점등
  (Cycle 300ms / Active 100ms), PWM 25/50/75%, 게인 256x(AGAIN=9), 적분시간 약 99.8ms(ATIME=99,
  ASTEP=358), dark 프레임 3:1, 패킷 v4(레코드 14B), BLE TX power -8dBm, 링버퍼 220샘플(연결 끊김 시
  약 22초 보관), 페어링 비활성(요구 조건 발생 시 개발), 온보드 flash 저장 없음.
  **적분시간 99.8ms와 Cycle/Active 300/100ms는 앱이 접속할 때마다 CONFIG를 써야 적용된다**(펌웨어 기본값은
  19.7ms, 100/100ms — 변경하지 않음). 앱 CONFIG 값은 `protocol_version_v4.md` §2 참고.
- **v0.1.39 기준 검증 이력(10/6~10/7)**: E7 야간 수면 측정 2회(각 약 4.6시간) 끊김·재부팅 없음, 순번 유실 0,
  센서 1·2 정상 출력, lit 간격 300ms. OTA 업데이트·롤백 재검증 6개 시나리오 통과(9A 1대, `Application/
  build_ota_test/OTA_테스트_결과.md`): 확정하지 않은 이미지 롤백, 불량 이미지 복귀, 서명 변조 이미지 미실행, 업로드
  70% 전원 차단 후 이전 버전 유지·재업로드, 정상 업데이트+확정 유지, v0.1.39 복구. 수면 데이터 종합 분석
  (Day1~Day6) 결론은 `docs/수면데이터_분석_종합결론_20261007.md`.
- **알려진 이슈 / 제약(릴리스 노트에 포함)**:
  - 센서 초기화 실패 시 그 센서를 재부팅 전까지 배제하고 raw를 0으로 전송한다. 상태 정보가 패킷에 없어 정상 0값과
    구분이 안 된다(v1.x 이월). 센서 2(U9)는 납땜 접촉 불량이 유력해 일부 프로토타입 보드에서 열풍기 리터치로
    복구됐다(`v1.0_센서2_초기화_실패_이슈.md`). 9A는 10/7부터 센서 1(NIR1)이 초기화 실패(원인 미확인).
  - 착용 이탈을 기기가 알지 못한다. 기기 시계가 앱 시계보다 약 +107ppm 빠르다(12시간에 약 4.4초, 샘플링 간격에는
    영향 없음). 12시간 연속 수면 검증은 아직 없다(최장 4.6시간). 배터리·도달 거리 실측은 0dBm 기준이다
    (v0.1.34~v0.1.38은 -8dBm 설정이 실제로는 적용되지 않았음, v0.1.39에서 수정).
  - OTA 서명키는 sysbuild 기본 디버그 키, 페어링/암호화 비활성(개발 단계 상태, 양산 전 결정 필요).
- **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d Application/build_v100 -p always`, 에러 없음,
  `version_MCUBOOT: "1.0.0+0"`, App Flash 75.05%(162,672B)/RAM 92.50%(60,624B), mcuboot Flash 70.74%.
  **실기기 확인(2026-10-08, 9A 1대, 부팅 로그 2회·앱 화면 기준):** OTA 업데이트 후 v1.0.0 부팅(배너 `v1.0.0-62c75a6b5a96`, 리셋 원인 0x0), 센서 1·2 모두 WHOAMI 0x24·SMUX 성공, 앱 접속 후 CONFIG(`05 01 19 32 4B 00 2C 01 64 00`) 적용·notify 구독·센서 1·2 데이터 수신, 전원 Off→On 후에도 v1.0.0 유지. 로그에서 `opcode 0xfc0e` 경고 없음(TX -8dBm 정상 적용). 연결 끊김 1회(`reason=0x08`)는 SWD 연결 불량 때문이라 이슈 아님(사용자 확인).
  **미검증:** 착용 상태 측정(연휴 중 진행 예정), 12시간 연속 수면, 여러 대 OTA, v1.0.0 기준 롤백 시나리오(코드가 v0.1.39와 같아 v0.1.39 기준 6종 결과를 그대로 사용).
- **앱 화면 관찰(펌웨어 변경 없음):** 앱의 `Sensor1 seq gap: prev=858 now=2147484507 (2147483648 samples lost)` 표시는 dark 프레임의 bit31(`0x80000000`)을 지우지 않고 순번을 비교해서 생긴 것으로 보인다(`2147484507 = 0x80000000 + 859`, 실제로는 858→859 연속). 앱에서 `& 0x7FFFFFFF` 처리 필요 — `protocol_version_v4.md` §3, `RELEASE_NOTES_v1.0.0.md` §6에 명시.
- **문서 정리(코드 변경 없음):** `RELEASE_NOTES_v1.0.0.md` 신규, `protocol_version_v4.md`·`tedneuro_firmware_integration.md`를 v1.0.0 기준으로 갱신, `tedneuro_partner_qa_response.md`·`app_handoff_*.md`에 v1.0.0 기준 정정 안내 추가, `v1.0_*.md` 3종과 `architecture.md` §0·§9에 v1.0.0 릴리스 상태 반영.

## v0.1.39 (2026-10-02, `dev/change_sampling_logic` 브랜치 — main 미병합, 긴급 패치)

- **[버그 수정] BLE TX power -8dBm이 실제로는 적용되지 않던 문제 수정** — 고장 디바이스
  9A의 PD2 디버깅 중 RTT 부팅 로그에서 `opcode 0xfc0e status 0x01`(Unknown HCI
  Command) 경고를 발견해 원인을 추적.
  - **원인**: `m_ble.c`가 쓰던 HCI VS Write TX Power Level 명령은 SoftDevice
    Controller가 `CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y`일 때만 지원한다
    (`nrf/subsys/bluetooth/controller/hci_internal.c`). 이 옵션이 꺼져 있어 명령이
    항상 거부됐고, 코드는 실패해도 경고만 남기고 계속 동작하는 구조라 눈에 띄지 않았다.
    그 결과 기기는 컨트롤러 기본값 **0dBm**으로 동작했다(v0.1.38 부팅 로그로 확인,
    v0.1.34~37은 같은 설정이라 동일했을 것으로 추정).
  - **수정**: `prj.conf`에 `CONFIG_BT_CTLR_TX_PWR_MINUS_8=y` 추가 — 컨트롤러 초기화 시
    `sdc_default_tx_power_set(-8)`로 전체 송신 기본값이 -8dBm이 된다. 항상 실패하던
    `m_ble.c`의 HCI VS 호출(`set_fixed_tx_power()`, 광고/연결 시 호출 2곳)과 관련
    상수/include를 제거.
  - **영향**: v0.1.34~v0.1.38 구간의 20m 도달 시험, 12시간 51분 배터리 실측, RSSI
    결과는 실제로는 **0dBm 기준**이다. -8dBm 기준 배터리 재측정은 추가로 진행 예정
    (0dBm에서 이미 12h 목표 달성이라 급하지 않고 차이 확인 수준), 수면 중 측정에서
    -8dBm 적용 후 연결 끊김이 생기는지 관찰 필요(지금까지는 끊김 없었음).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application`, 에러 없음,
    App Flash 75.05%/RAM 92.50%(RAM 변화 없음, 죽은 코드 제거로 Flash 약 0.2% 감소).
    **하드웨어 검증(부분)**: 같은 위치에서 OTA로 0dBm/-8dBm 빌드만 바꿔 연결 상태
    RSSI를 비교 — 0dBm -44~-52dBm, -8dBm -52~-60dBm(중간값 약 8dB 차). 컨트롤러의
    실제 출력 값을 직접 읽는 방법은 이 구성에서 지원되지 않아(`0x2007` 명령 미지원)
    RSSI 비교가 유일한 확인. 수면 중 안정성/배터리는 미검증.
  - 버전 표기: `VERSION`, `config_app.h FW_VERSION_PATCH`, `CONFIG_BT_DEVICE_NAME`을
    함께 v0.1.39로 올림.

## v0.1.38 (2026-10-01, `dev/change_sampling_logic` 브랜치 — main 미병합, v0.1.37 재수정)

- **[버그 수정] dark 보정 sleep 계산이 nominal 적분시간 가정이라 과소/과대
  보정되던 문제 수정** — v0.1.37로 BLE 데이터(0.1초 단위라 수십ms 오차는
  안 보임, 아래 참고)는 통과했으나, 사용자가 오실로스코프(R17, LED1 구동선
  직접 프로빙, Cycle 300ms 커서 측정은 정확히 일치)로 재검증하는 과정에서
  "active 폭이 공칭 100ms보다 길다"와 "3번째 active마다 다음 active와
  미묘하게 딜레이가 생긴다"를 발견·제보.
  - **원인**: v0.1.37의 보정 sleep 계산이 `lit_and_dark_ms = 2 × (적분시간
    nominal값)`으로 "lit/dark가 각각 정확히 적분시간만큼만 걸린다"고
    가정했는데, 실제 `acquire_common()`은 적분시간 외에 트리거+AVALID
    폴링+센서 2개 I2C 읽기 오버헤드가 더 붙어서 lit 하나가 공칭보다 더
    걸린다(오실로스코프 active 폭 실측으로 확인). 그 결과 보정 sleep이
    과도해져 dark가 낀 구간에서 3번째 active마다 cycle_ms보다 지연됨 —
    v0.1.37이 고치려던 문제가 축소된 형태로 재발한 것. BLE 데이터
    `device_ts`가 0.1초 단위로만 기록돼 이 수십ms 오차가 "지터 0"으로
    보였을 뿐 실제로는 있었다(v0.1.37 항목에 정정 추가).
  - **수정**: nominal 적분시간으로 추정하는 대신, 이번 활성 tick이 실제로
    시작된 시각을 `k_uptime_get_32()`로 기록해두고(`active_tick_start_ms`),
    dark까지 끝난 시점에 실제 경과시간을 직접 측정해서
    `cycle_ms - 실제경과시간`만큼만 재우도록 변경 — I2C 오버헤드가 얼마든
    실측값 기준으로 정확히 보정된다(`m_i2c.c`, `is_gate_active_tick()` 블록
    상단 참고).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d
    Application/build -p always` 성공, App Flash 75.25%(+16B)/RAM 92.50%
    (변화 없음). **하드웨어 검증 미실시** — 이번엔 오실로스코프(R17)로
    active 간격이 3번째마다 밀리지 않고 항상 정확히 300ms 간격인지 직접
    재확인 필요. `main` 병합은 이 검증 통과 후 진행.

## v0.1.37 (2026-09-30, `dev/change_sampling_logic` 브랜치 — main 미병합)

- **[버그 수정 시도, 하드웨어 검증 전 임시] dark 프레임 삽입으로 인한 lit 샘플링
  주기 왜곡 수정** — 사용자가 12h 배터리 전력 최적화(v0.1.34, dark:lit
  10:1→3:1) 적용 후 실사용 로그(`2026-09-30_01-37-56_fNIRS_RAW_DATA.csv`)에서
  "샘플링 주기(300ms)가 안 지켜진다"고 제보, 실측 데이터로 원인 확정.
  - **원인**: dark 프레임은 lit과 같은 tick 안에서 lit 직후 추가로
    트리거+적분되는 구조라(2026-09-28 설계), lit3(타임스탬프 T) → dark(T+
    적분시간, 예 100ms) → 다음 lit까지의 실제 간격이 dark가 없을 때의 정상
    lit-lit 간격(300ms, 지터 0으로 실측 확인)과 달리 **400ms로 벌어짐**(lit3→
    dark 100ms + dark 완료 후 자연 대기 200ms). dark가 안 낀 lit-lit 구간은
    전부 정확히 300ms로, 랜덤한 레이스 컨디션이 아니라 100% 결정론적인 구조적
    결과임을 19,344회 전부 지터 0으로 확인.
  - **수정**: dark 트리거+적분 완료 직후, "다음 lit이 필요할 때까지" 세마포어/
    tick 카운터 기반 대기에 맡기지 않고, `cycle_ms - 2×integration_ms`만큼만
    명시적으로 재운 뒤 그 자리에서 바로 다음 lit을 강제 수행 — lit3 시작
    시각 기준 정확히 cycle_ms(예: 300ms) 뒤에 오도록 강제한다. 이후
    `s_gate_tick_count`를 `cycle_ticks`만큼 앞당겨서(공통 trailing ++와 합쳐
    총 cycle_ticks+1회분) 다음 is_gate_active_tick() 판정이 실제 경과 시간과
    어긋나지 않게 정리. 세마포어는 일부러 비우지 않음(시뮬레이션 결과, 비우면
    오히려 100ms 더 밀리는 부작용 확인 — 남은 신호 1개는 다음 비활성 tick이
    빠르게 자연 소비하도록 방치하는 게 맞음).
  - **검증 방법**: 코드 리뷰 + 수기 시뮬레이션(여러 차례 재계산, 첫 시도는
    계산 오류로 폐기 후 재설계)으로 이론상 lit-lit 간격이 항상 cycle_ms로
    유지될 것으로 예상.
  - **하드웨어 검증 완료(2026-09-30)**: `2026-09-30_13-12-00_fNIRS_RAW_DATA.csv`
    (1261행) 실측 결과 — lit→lit 300ms(630회 전부 지터 0), lit→dark
    100ms(315회 전부 지터 0), dark→lit 200ms(315회 전부 지터 0), dark를
    포함한 lit-dark-lit 전체 구간도 정확히 300ms(315회 전부 지터 0) —
    **의도한 대로 lit-lit 간격이 dark 유무와 무관하게 항상 정확히 300ms로
    유지됨을 실기로 확인**. `main`으로 병합.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d
    Application/build -p always` 성공, App Flash 75.24%(+184B)/RAM 92.50%
    (변화 없음).
  - **[정정, 2026-10-01] 위 하드웨어 검증 결과가 불충분했음이 밝혀짐** — BLE
    데이터의 `device_ts` 컬럼이 0.1초(100ms) 단위로만 기록돼, 수십ms 수준의
    오차는 "지터 0"으로 보여도 실제로는 감지 못 하는 측정이었다. 사용자가
    오실로스코프(R17, LED1 구동선)로 직접 재본 결과 3번째 active마다 실제로
    딜레이가 있는 것으로 재확인 — 상세 원인/재수정은 v0.1.38 참고.

## v0.1.36 (2026-09-29)

- **[정리] LED duty 펌웨어 기본값을 확정값(25/50/75%)으로 변경** — `m_i2c.c
  s_led_duty_permille` 초기값이 그동안 기존 placeholder(500/500/500‰=50/50/50%)로
  남아있었다 — 2026-09-28에 "매뉴얼 조합 25/50/75%"로 고정 운용 확정된 이후에도
  기본값 자체는 안 바뀌어서, 앱이 `AS7341_CONFIG`를 보내기 전까지는 확정값이
  아닌 50/50/50%로 측정되는 상태였다. 250/500/750‰(25/50/75%, 640/680/950nm)로
  변경 — Ambient light 정량 검증(레퍼런스 대비 비교) 실측을 앞두고, 앱이 CONFIG를
  깜빡 안 보내도 확정값 그대로 측정되도록 사용자가 요청.
  **빌드 검증 완료**: App Flash 75.16%/RAM 92.50%(변화 없음, 초기값 상수만 변경).
  **하드웨어 검증 미실시**.

## v0.1.35 (2026-09-29)

- **[전력 최적화 조정] BLE batch flush 주기 3000ms → 2000ms** — v0.1.34에서 3000ms로
  정했던 걸 사용자가 "먼저 2000ms로 해보자"고 결정, 더 보수적인 값으로 조정
  (`m_ble.c BLE_BATCH_FLUSH_INTERVAL_MS`). dark 3:1 기준 샘플레이트(~4.4/s)에서
  한 번에 약 9개(17개 용량 중)를 채워 보냄 — 3000ms(약 13개)보다는 덜 채우지만
  실시간성 저하는 더 적음. 실측 후 필요하면 3000ms 방향으로 더 늘릴 수 있음.
  **빌드 검증 완료**: App Flash 75.16%/RAM 92.50%(변화 없음, 상수값만 변경).
  **하드웨어 검증 미실시**.

## v0.1.34 (2026-09-29)

- **[전력 최적화 + 신호처리] dark:lit 비율 조정 + 배터리 12h 목표 대응** — 어제
  실측 결과 10:1 비율(v0.1.24)로 밤새 운용 시 약 10.5~11h(목표 8h는 이미 초과
  달성, DCDC 효과 큼) 확인. 사용자가 dark 기준점 정밀도를 높이면서도 새 목표
  12h를 지키고 싶다고 해서 검토 후 3건 조정.
  - **dark:lit 비율 10:1 → 3:1** (`config_app.h DARK_FRAME_LIT_INTERVAL`). dark
    프레임은 별도 tick이 아니라 활성 tick 안에서 lit 직후 추가 트리거+적분하는
    구조라(m_i2c.c), 1:1까지 가면 AS7341/I2C 활성 듀티가 약 36%→약 66%로 거의
    2배가 돼 12h 목표에 오히려 역행할 위험이 크다고 판단 — 3:1(활성 듀티
    약 43%)로 절충. dark/ambient 드리프트가 느리다는 기존 전제(v0.1.24)는 유지.
  - **BLE TX power -8dBm 고정 구현** (`m_ble.c set_fixed_tx_power()`) — 이전부터
    TODO(open-item)로 남아있던 S2 정책(architecture.md §2.4) 항목. SoftDevice
    Controller의 Zephyr HCI vendor-specific 확장(`BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL`,
    `zephyr/samples/bluetooth/hci_pwr_ctrl` 패턴 참고)으로 advertising
    핸들(`start_advertising()`)과 매 connection 핸들(`on_connected()`)에 각각
    적용. 착용형 기기라 폰과의 거리가 통상 1m 이내라는 전제.
  - **BLE batch flush 주기 1000ms → 3000ms** (`m_ble.c BLE_BATCH_FLUSH_INTERVAL_MS`)
    — dark 3:1 기준 샘플레이트(~4.4/s)에서 1초 주기는 배치 용량(17개)의 약
    4~5개만 채운 채 매초 무선을 깨우고 있었다. 3초로 늘려 한 번에 약 13개를
    채워 보내 무선 송신 횟수를 줄임(오버나이트 수면측정 용도라 실시간성 저하는
    수용 가능하다고 판단).
  - **미검증(estimate)**: 위 활성 듀티/전력 수치는 실측이 아니라 구조적 계산
    추정치다 — 실제 mW 단위 효과는 전류 프로파일 실측(파워 프로파일러 등) 필요.
  - **RSSI/재연결 안정성 하드웨어 검증 완료(2026-09-29, v0.1.35 보드)** — 사용자가
    실기로 확인: **약 20m 거리까지 연결 끊김 없이 정상 동작**. -8dBm 고정이
    착용형 기기의 통상 사용 거리(실내, 1m 내외)에서 안정적으로 동작함을 확인 —
    지나치게 낮춘 게 아니었음이 실기로 확인됨.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d
    Application/build -p always` 성공, App Flash 75.16%(+524B)/RAM 92.50%
    (변화 없음). **하드웨어 검증 미실시** — 다음 세션에서 (1) -8dBm에서도
    통상 착용 거리 내 정상 연결/재연결 확인, (2) 새 설정(3:1 dark + TX -8dBm +
    flush 3s) 조합으로 하룻밤 실사용 배터리 실측(목표 12h 대비) 필요.

## v0.1.33 (2026-09-29)

- **[안전 아키텍처 변경, 고위험] 모듈 오류 재부팅 에스컬레이션 도입** — 사용자가
  TedreamS2(최신 디바이스)의 재시도 로직("모듈 범위 3회 재시도 → 실패 시 재부팅,
  이를 3회 반복 → 그래도 실패하면 전원 off")을 제시하고, 우리 구조/제약에 맞게
  검토·변형해서 적용할지 논의 후 결정.
  - **검토 결과(그대로 이식 불가한 지점 3곳)**:
    1. 4단계 "전원 off"는 이 보드가 물리 SW1만 있고 SW로 전원을 끊을 방법이 없어
       그대로 구현 불가 — **"더 이상 재부팅하지 않는 영구 정지(물리 전원 재투입
       전까지 유지)"**로 대체.
    2. 재부팅 횟수를 재부팅을 넘어 기억할 저장소가 필요 — RAM은 `sys_reboot()`
       순간 사라짐. **GPREGRET**(nRF52 POR 전까지 지워지지 않는 레지스터)을
       채택. `architecture.md` §3에 예전에 "GPREGRET 상태복원 채택 안 함"
       결정이 있었으나, 그건 "이전 동작 모드 복원" 용도였고 이번은 "횟수만
       카운트"하는 다른 용도라 재검토 후 사용자 승인받아 적용.
    3. "모듈 범위"를 우리 구조 어디에 적용할지 — AS7341 센서별 streak/init()
       내부 재시도가 이미 존재해 이중 계층이 될 수 있었음. **사용자 결정: I2C
       태스크/시스템 전체로 확장** — 즉 기존에 `CTRL_STATE_DEGRADED`로 곧장
       래치되던 모든 경로(I2C_TIMEOUT/NACK, BLE_TX_FAILED/RETRY_EXCEEDED,
       default)에 적용.
  - **구현**: `m_ctrl.c`에 `escalate_or_reboot()` 추가 — `handle_error()`가
    이 경로로 들어오면 GPREGRET 카운트(`CTRL_MODULE_FAULT_REBOOT_MAX`=3,
    config_app.h)가 상한 미만일 때는 카운트를 올리고 `m_ctrl_request_reset
    (CTRL_RESET_REASON_FAULT)`로 재부팅, 상한에 도달했을 때만 기존처럼
    `CTRL_STATE_DEGRADED`로 영구 전환. GPREGRET1(`DT_NODELABEL(gpregret1)`,
    nrf52832.dtsi에 이미 `status="okay"`로 정의돼 있어 overlay 불필요)을
    Zephyr `retained_mem` API(`retained_mem_read/write`)로 접근,
    `prj.conf`에 `CONFIG_RETAINED_MEM=y` 추가. `log_reset_cause()`에서 부팅 시
    현재 카운트를 진단용으로 로그.
  - **주의(그대로 남겨둔 TedreamS2 원 로직의 특성)**: 이 카운트는 부팅 세션
    내내 누적되며, 재부팅이 성공적으로 복구돼도 리셋되지 않는다 — 서로 무관한
    오류가 3번 쌓여도 마지막엔 영구 정지된다. 이 동작이 의도한 것인지(TedreamS2
    원 로직 그대로 유지) 재확인 필요.
  - **[2026-09-29] 안전 철학 정합성 검토 완료(사용자 판단으로 종료)**: 이
    재부팅-재시도 방식이 기존 v0.1.31까지의 "고장 확인 시 즉시 영구 안전상태,
    자동 재시도 없음" 원칙과 인증 관점에서 정합적인지가 미해결 항목이었는데,
    이미 안전인증을 받은 TedreamS2(최신 디바이스)가 동일한 "제한된 횟수까지
    재시도 후 영구 정지" 철학을 쓰고 있다는 선례를 근거로 적용 가능하다고
    판단해 종료(`architecture.md` §11 항목5-2). 정식 인증 심사 시에는 TedreamS2
    인증 근거 문서와의 논리적 동일성 재확인이 필요하다.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d
    Application/build -p always` 성공, App Flash 74.92%(+880B)/RAM 92.50%
    (변화 없음). **[2026-09-29] 하드웨어 검증 종료(사용자 판단)** — 3회 연속
    실패→재부팅 3회 반복→영구 정지, 물리 전원 재투입 시 카운트 리셋의 개별
    실기 재현은 하지 않았으나, 위 안전 철학 검토 완료와 함께 이 항목도
    마무리하기로 결정.

## v0.1.32 (2026-09-29)

- **[치명적 버그 수정] 부팅 시 AS7341 한쪽 센서 초기화 실패 → 무한 Watchdog 리셋 루프** —
  사용자가 R3(SCL)/R4(SDA) 풀업 저항을 실제로 떼어내고(I2C 라인 오픈 회로 fault
  injection 테스트) 재부팅한 RTT 로그로 발견: NIR1 WHOAMI 읽기가 `i2c err=-116`으로
  계속 실패하다 짧게 끊기고, 곧바로 새 부팅 사이클이 다시 시작되는 패턴이 무한
  반복됨.
  - **원인**: `i2c_init()`이 `m_i2c_as7341_init(NIR1)`과 `m_i2c_as7341_init(NIR2)`를
    순서대로 호출하면서 **하나라도 실패하면 즉시 `return err`** — 나머지 센서 init,
    gain/integration 설정, 부팅 버전 점멸, LED 모드 세팅까지 전부 건너뛰고
    `i2c_init()`이 실패로 리턴됨. 이걸 받은 `m_i2c_task_entry()`는
    `m_ctrl_report_error()`로 안전상태 래치 후 `k_sleep(K_FOREVER)`로 태스크를
    영구히 재웠고, 이후 이 태스크는 `m_ctrl_notify_alive(CTRL_ALIVE_I2C)`를 다시는
    호출하지 않음 — Watchdog(4초, I2C/BLE 두 alive 소스 모두 필요)이 계속 stale로
    판정해 SoC를 리셋, 리셋해도 똑같은 자리에서 또 실패 → 무한 반복.
    v0.1.27의 런타임 fault recovery(`recover_as7341_or_escalate()`,
    `s_i2c_fault_streak[NIR_SENSOR_COUNT]`)는 이미 측정 중이던 센서가 일시적으로
    실패하는 경우만 다루고 있었고, **부팅 시점의 all-or-nothing 구조는 별개의,
    이번에 처음 발견된 버그**였음.
  - **수정**: `i2c_init()`이 NIR1/NIR2를 서로 독립적으로 시도하도록 변경 — 새 정적
    배열 `s_as7341_ready[NIR_SENSOR_COUNT]`로 센서별 초기화 성공 여부를 기록하고,
    한쪽만 실패하면 그 센서만 배제한 채(로그만 남기고 `m_ctrl_report_error()` 호출
    없이) 남은 센서로 gain/integration 설정·부팅 점멸·LED 모드 세팅까지 정상
    진행한다. **둘 다 실패했을 때만**(`ready_count == 0`) 기존처럼
    `MODULE_ERR_NOT_INITIALIZED`를 리턴해 안전상태로 전환한다(측정할 게 아예 없는
    진짜 치명적 상황이므로 이 경우는 그대로 유지). `acquire_common()`의 트리거
    루프도 `s_as7341_ready[i]`가 false인 센서는 매 tick 재시도하지 않고
    `raw[i]`를 0으로 유지한 채 건너뛰도록 수정(불필요한 500ms I2C 타임아웃 반복
    방지).
  - **런타임 fault recovery(v0.1.27)와의 관계**: 부팅 후 정상 동작 중이던 센서가
    나중에 I2C 실패하는 경우는 여전히 `recover_as7341_or_escalate()`가 처리(3회
    연속 실패해야 `CTRL_STATE_DEGRADED` 에스컬레이션). 이번 수정은 그 앞
    단계(부팅 시점)의 구멍만 메운 것.
  - TODO(open-item): 부팅 시 배제된 센서를 런타임 중 주기적으로 재프로브하는 로직은
    아직 없음 — 물리적으로 복구(R3/R4 재납땜 등)돼도 재부팅 전까지는 계속
    배제 상태로 남는다. 필요성이 확인되면 추가.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -d Application/build
    -p always` 성공, App Flash 74.51%/RAM 92.50%(v0.1.31 대비 거의 변화 없음 — static
    bool 배열 2B 추가 수준).
  - **하드웨어 검증 완료(2026-09-29, R3/R4 제거 상태 그대로 재플래시)**: RTT 로그로
    확인 — NIR1(`i2c@40004000`) WHOAMI=0x24+SMUX 구성 정상, NIR2(`i2c@40003000`,
    R3/R4 제거로 통신 단절)는 `WHOAMI read failed ... (i2c err=-116)` →
    `AS7341(1) 부팅 초기화 실패 — 이 센서는 배제하고 계속 진행 (err=5)` 로그
    한 줄만 남기고 부팅이 멈추지 않음. 이어서 BLE 스택 초기화·advertising
    (`TedNeuro_v0.1.32`)까지 정상 진행 확인 — **무한 Watchdog 리셋 루프 재발 없음**,
    의도한 대로 수정됨.

## v0.1.31 (2026-09-28)

- **[버그 수정] Watchdog이 SWD 디버거(RTT) 연동 중에만 오발동하던 문제 해결** —
  사용자가 실기로 재현/제보: "BLE만 연동하면 안 걸리는데, RTT까지 같이 연동하면
  Watchdog 리셋이 걸린다."
  - **원인**: `m_ctrl.c watchdog_init()`이 `wdt_setup(dev, 0)`으로 옵션 없이 워치독을
    세팅하고 있었다 — Zephyr nRF WDT 드라이버(`wdt_nrfx.c`) 확인 결과, `WDT_OPT_
    PAUSE_HALTED_BY_DBG` 없이 세팅하면 `NRF_WDT_BEHAVIOUR_RUN_HALT_MASK`가 켜져서
    **SWD 디버거가 코어를 halt시켜도 워치독 카운터가 계속 돈다.** RTT/디버그 세션이
    붙어있으면 디버거가 짧게 코어를 halt하는 경우가 생기는데(메모리/변수 조회 등),
    애플리케이션은 실제로 멈춘 적이 없어도 이 halt 시간들이 누적되면 4초
    (`WATCHDOG_TIMEOUT_MS`)를 넘겨 워치독이 리셋시킨다 — 9/28 포화 테스트 로그에서
    실제로 이 패턴 확인(포화 지속 중 로그 한 줄이 끊기면서 재부팅).
  - **수정**: `wdt_setup(s_wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG)`로 변경 — 디버거로
    halt된 구간만 워치독 카운터도 같이 멈춘다. 디버거가 안 붙어있는 정상/양산
    동작에서는 이 옵션이 전혀 영향을 주지 않아, "진짜로 멈춘 태스크 감지"라는
    워치독 본래 목적은 그대로 유지된다 — 개발 편의성 개선이지 안전 기능 약화가
    아니다.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.31+0"`, 에러 없음, App Flash 74.39%/RAM 92.50%(변화 없음).
    **하드웨어 검증 미실시** — 다음 세션에서 RTT+BLE 동시 연동 상태로 포화 테스트를
    재현해서 워치독 리셋이 더 이상 안 걸리는지 확인 필요.

## v0.1.30 (2026-09-28)

- **[전력 최적화] DCDC 레귤레이터 활성화** — 내부 전압 레귤레이터를 기본값 LDO에서
  DCDC(벅 컨버터)로 전환. 보드에 이미 DCDC용 인덕터가 실장돼 있음을 `pinmap.md` §5로
  재확인(L3/L4, pin47) — 하드웨어는 준비돼 있었고 소프트웨어에서 안 켜고 있었을 뿐.
  데이터시트(nRF52832 PS) 기준 TX 전류가 거의 절반(0dBm 기준 LDO 11.6mA→DCDC
  5.3mA, -8dBm 기준 8.4mA→3.8mA)으로 줄어든다 — §8 배터리 8h 목표와 직결.
  - **시행착오**: `CONFIG_SOC_DCDC_NRF52X=y`를 prj.conf에 먼저 시도했으나 이 NCS
    버전(Zephyr 4.4)에서 **deprecated(prompt 없는 내부 심볼)**라 `west build`가
    Kconfig 에러로 즉시 거부(`is not directly user-configurable`) — devicetree로
    설정하는 게 맞는 방식임을 확인(Kconfig help 텍스트에 명시돼 있었음).
  - **정정 적용**: `Application/app.overlay`에 `&reg { regulator-initial-mode =
    <NRF5X_REG_MODE_DCDC>; };` 추가. 생성된 `zephyr.dts`에서
    `regulator-initial-mode = <0x1>`(DCDC)로 정확히 반영된 것 확인.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.30+0"`, 에러 없음, App Flash 74.39%/RAM 92.50%(변화 없음
    — 레지스터 설정이라 코드 크기 영향 없음). **하드웨어 검증 완전 미실시** — 전원부
    전체에 영향을 주는 변경이라 아래 검증 계획대로 반드시 실기 확인 필요.

### DCDC 활성화 검증 계획 (하드웨어 필요, 전부 미실시)

1. **전류소비 실측(AT-06, 가장 중요)** — 배터리 라인에 전류계(또는 Power Profiler
   Kit II 등) 직렬 연결 후 DCDC on/off 두 빌드로 비교:
   - BLE TX 순간 전류(peak): 데이터시트 예측치(0dBm 11.6mA→5.3mA)와 실측 대조
   - 전체 평균 전류(정상 측정 사이클 기준): duty cycle 반영한 실제 배터리 수명 개선폭 확인
2. **전압 리플/노이즈 확인** — 오실로스코프로 VDD 라인 확인, DCDC 스위칭 주파수
   노이즈가 AS7341 I2C 신호나 ADC 판독값에 영향을 주는지(특히 낮은 광량 채널에서
   노이즈 플로어 상승 여부) 확인
3. **기능 회귀 확인** — I2C(AS7341 WHOAMI/SMUX/read_raw 정상), BLE(광고/연결/notify
   정상), RTC tick 타이밍(100ms 정확도 유지) 전부 DCDC on 상태에서 재확인
4. **BLE 연결 안정성** — 평소처럼 몇 분 이상 연결 유지하며 disconnect/재연결 이상
   없는지(DCDC 스위칭 노이즈가 라디오에 영향 줄 가능성 배제)

**1차 실측 시도(2026-09-29)** — v0.1.35 기준 DCDC ON/OFF DFU 파일 2개를 만들어
파워서플라이로 각각 실측(`TedNeuro_v0.1.35_DCDC_ON_dfu.zip`/`_OFF_dfu.zip`,
`Application/build_dcdc_test_output/`). 결과: advertising 시 약 5mA, LED
측정 동작 시(25/50/75%, cycle 300/active 100) 약 28mA — **ON/OFF 두 조건에서
거의 동일하게 나옴**.
- **결론: "DCDC 효과 없음"이 아니라 이 측정 방법의 해상도 한계로 판단.** 두
  가지 이유:
  1. LED 동작 시 28mA 중 대부분(계산상 약 21mA, LED1 0.56mA+LED2 12.0mA+
     LED3 8.33mA — 스키매틱 R15/R16/R19+Vf 기준 `I=(VDD-Vf-Vce_sat)/R`로
     듀티 가중평균 계산)은 **LED 구동 전류**다. LED는 VDD3.3V→저항→LED→
     트랜지스터(GND) 경로로 배터리 전압을 직접 쓰고 SoC 내부 레귤레이터를
     거치지 않아 **DCDC 설정과 원천적으로 무관한 전류**다. 즉 이 구간에서
     ON/OFF가 같게 나오는 건 당연한 결과다.
  2. advertising 구간의 DCDC 효과는 라디오 TX 순간(수백 µs 펄스)에만
     나타나는데, 일반 파워서플라이 전류계는 평균/완만한 응답이라 이런 짧은
     스파이크를 못 잡을 가능성이 높다 — "차이가 없다"가 아니라 "이 장비로는
     안 보인다"일 수 있음.
- **재검증 방법 제안(다음 세션)**: (a) Nordic Power Profiler Kit II 등 µs
  단위 전류 프로파일러로 TX 펄스 파형 자체를 캡처해서 비교, 또는 (b) 순간
  전류가 아니라 배터리로 장시간(몇 시간) 실사용한 총 러닝타임 차이로 간접
  확인(§8 12h 배터리 실측과 통합 가능 — 작은 평균 전류 차이도 누적되면
  측정 가능).
- **부가 발견**: LED2(680nm)가 50Ω 저항+낮은 Vf(1.8V) 조합이라 듀티 50%
  기준 평균 12.0mA로 세 LED 중 압도적으로 큰 소비 비중을 차지 — 배터리
  예산에서 LED 전류가 지배적임을 보여줌(향후 전력 최적화 시 참고, 지금은
  신호 품질 확정값이라 임의 변경 안 함).
5. 4개 전부 이상 없으면 DCDC 확정, 문제 발견 시 `app.overlay`의 `&reg` 오버라이드만
   제거하면 즉시 LDO로 롤백 가능(devicetree 한 블록이라 되돌리기 쉬움).

## v0.1.29 (2026-09-28)

- **[정정] RTT 로그 백엔드를 BLOCK→OVERWRITE 모드로 전환** — 사용자가 제공한 RTT
  동작 원리 자료를 우리 NCS v3.4.0 실제 Kconfig(`log_backend_rtt.c`,
  `Kconfig.rtt`)와 교차검증 후 반영.
  - **발견**: `CONFIG_LOG_BACKEND_RTT_MODE`를 지금까지 한 번도 명시적으로 설정한 적이
    없어 Zephyr 기본값인 `MODE_BLOCK`으로 동작 중이었음(`.config`로 확인) — 매 로그
    메시지마다 최대 4회×5ms 재시도 후 드롭. `CONFIG_LOG_MODE_DEFERRED` 덕분에 이게
    m_i2c/m_ble/m_ctrl 태스크를 막지는 않지만(로깅 전용 스레드만 재시도), J-Link
    연결 없이 오래 방치하면 사실상 거의 모든 로그가 버려져 **"한참 뒤에 연결해서
    그동안 쌓인 걸 본다"는 실사용 시나리오에서 버퍼에 남는 게 거의 없었음**.
  - **수정**: `CONFIG_LOG_BACKEND_RTT_MODE_OVERWRITE=y`로 전환. 이 모드는
    `SEGGER_RTT_WriteWithOverwriteNoLock()`을 직접 호출해(재시도 없이 즉시 기록)
    버퍼가 차면 가장 오래된 로그부터 덮어쓴다 — 장시간 무인 방치(watchdog fault
    injection/sensor fault recovery/배터리 자연방전 검증 등) 후 이상 발생 직전
    로그를 보는 우리 용도에 정확히 맞음.
  - **정정(사용자 제공 자료 대조)**: 제공된 자료의 `CONFIG_LOG_BACKEND_RTT_BUFFER_SIZE`
    설정 제안은 우리 구성에는 적용되지 않음 — 이 옵션은 `LOG_BACKEND_RTT_BUFFER>0`
    (전용 버퍼 인덱스)일 때만 적용되는데 우리는 기본 버퍼 인덱스 0을 그대로 쓰고
    있어(`LOG_BACKEND_RTT_BUFFER=0`), 실제 버퍼 크기는 기존처럼
    `CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=4096`이 결정한다(변경 없음).
  - 리셋/크래시 원인 자체는 이 변경과 무관하게 기존 `m_ctrl.c log_reset_cause()`
    (RESETREAS 레지스터, 다음 부팅 시 확인)가 계속 담당 — RTT는 "전원 유지된 채
    방치" 시나리오만 커버한다는 점은 그대로.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.29+0"`, 에러 없음, App Flash 74.39%(-128B, BLOCK 모드의
    재시도 로직 제거)/RAM 92.50%(변화 없음). **하드웨어 검증 미실시** — 다음
    세션에서 J-Link 없이 장시간 방치 후 연결해서 최근 로그가 실제로 남아있는지
    확인 필요.

## v0.1.28 (2026-09-28)

- **[정리] 메모리/RAM 최적화 1차 — 저위험 항목만 실행**(`memory_ram_optimization_plan.md`
  참고, `west build -t ram_report` 실측 기반 계획). 고위험 항목(mcumgr netbuf 축소,
  OTA 재검증 필요)은 보류하고 이번엔 안전한 정리만 진행:
  - **미사용 모듈 삭제**: `m_ble_batch.c`/`m_ble_batch.h` — `m_ble.c`가 이미 자체
    배칭 로직(`s_batch`/`s_batch_count`)으로 완전히 대체했는데도 `CMakeLists.txt`에는
    계속 남아있던 죽은 파일. 어디서도 호출되지 않는 것을 확인 후 파일과
    `CMakeLists.txt` 참조 모두 제거.
  - **해소된 TEMP_ 디버그 훅 3개 제거**: `TEMP_LED_STATIC_ALL_ON_TEST`(D3/D4 LED
    미점등 진단, 2026-09-14 해결 완료), `TEMP_RTC_TICK_LOG_TEST`(RTC tick 100ms 실측,
    검증 완료), `TEMP_BLE_DISABLE_TEST`(BLE 스택 격리 진단, 원인 확정 완료) — 전부
    0으로 비활성화된 채 방치돼있던 초기 개발용 임시 코드였다. `config_app.h`의
    매크로 정의와 `m_i2c.c`/`m_ble.c`의 `#if` 블록을 함께 제거.
  - **유지한 것**: `TEMP_WATCHDOG_FAULT_INJECT_TEST`(AT-10 재사용 예정),
    `TEMP_AS7341_READ_TEST`/`TEMP_AS7341_RAW_DBG_LOG`(이번 승인 범위 밖, 계획 문서에
    후속 항목으로 기록만).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.28+0"`, 에러 없음, App Flash 74.45%/RAM 92.50%
    (예상대로 변화 없음 — 코드 정리 목적이라 RAM 절감은 이번 항목의 목표가 아니었음,
    가장 큰 절감 여지인 mcumgr netbuf는 고위험이라 보류). **하드웨어 검증 미실시.**

## v0.1.27 (2026-09-28)

- **[신규 기능] Sensor(AS7341) fault recovery 구현** — Gantt "Watchdog + sensor fault
  recovery"(R3-1) 항목 중 watchdog(SoC 하드웨어 리셋)은 이미 완료돼 있었고 sensor fault
  recovery만 남아있던 상태를 마무리.
  - `m_i2c.c`에 `recover_as7341_or_escalate()` 추가 — AS7341 트리거/읽기 중 I2C 통신
    실패(`MODULE_ERR_I2C_TIMEOUT`)가 발생하면 즉시 CTRL에 보고해 `CTRL_STATE_DEGRADED`
    (단일고장 안전상태, 재부팅 전까지 래치)로 보내지 않고, 먼저 **재초기화**
    (`m_i2c_as7341_init()` — WHOAMI 재확인+`i2c_recover_bus()`+SMUX 재구성)로 자체 복구를
    시도한다. 재초기화 성공 시 이번 tick 샘플만 유실 처리하고 다음 tick부터 정상
    복귀(gain/integration time도 재적용). 연속 실패가 `I2C_FAULT_RECOVERY_MAX_ATTEMPTS`
    (=3)회 누적됐을 때만 진짜 하드웨어 고장으로 판단해 기존처럼 `CTRL_STATE_DEGRADED`로
    에스컬레이션 — 단일고장 안전상태 래치 원칙은 그대로 유지하되, 일시적 I2C 글리치까지
    영구 정지시키지는 않게 됨.
  - 트리거 실패한 센서는 같은 tick에 읽기(read_raw)를 스킵해 같은 tick에 recovery를
    두 번 시도하지 않도록 정리.
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.27+0"`, 에러 없음, App Flash 74.45%(+344B)/RAM 92.50%
    (변화 없음).
  - **하드웨어 검증 완료(2026-09-29, v0.1.32 보드에서 R3(SCL)-GND 순간 쇼트/해제로
    재현)** — 두 센서 다 정상 부팅된 상태에서 NIR2 쪽 SCL을 짧게 쇼트했다 뗐더니
    RTT 로그로 `WHOAMI read failed(err=-116)` → `AS7341(1) I2C fault 복구 실패
    (streak=1/3)` → 다음 tick `WHOAMI=0x24`+`SMUX 구성 성공` → `AS7341(1) I2C fault
    복구 성공(재초기화, 직전 streak=1)` 확인, 같은 패턴이 이후 한 번 더(streak=2까지)
    재현됨 — **(1) 재초기화 자동 복구는 실기로 확인**. 이 동안 NIR1은 전혀 영향받지
    않고 정상 측정 계속(센서 독립성도 함께 확인). **(2) 3회 연속 실패 시
    에스컬레이션 경로는 이번 테스트에서 매번 3회 채우기 전에 복구돼서 그 경계값을
    직접 재현하지는 못했으나, **[2026-09-29] 검증 종료(사용자 판단)** — (1)의
    정상 복구 경로 확인 + v0.1.33의 재부팅 에스컬레이션 도입으로 이 항목을
    마무리.

## v0.1.26 (2026-09-28)

- **[신규 기능] AS7341 STATUS2(ASAT/AVALID) 기반 포화 검출 구현** — Gantt "2. Acquisition
  & Sync" §4번 항목(그동안 0%) 마무리. datasheet(DS000504 v3-00, §10 Figure 64, 페이지
  44-45)를 직접 대조해 STATUS2(0xA3) 레지스터의 정확한 비트 위치를 확인:
  bit4=ASAT_DIGITAL(적분시간 기준 카운터 최댓값 도달), bit3=ASAT_ANALOG(광량이 스펙트럴
  아날로그 회로 적분 한계 초과) — 기존에 이미 검증돼 쓰이고 있던 bit6=AVALID와 함께
  `m_i2c_as7341.h`에 추가.
  - `m_i2c_as7341_read_raw()`에 `bool *out_saturated` 파라미터 추가(NULL 허용) —
    AVALID가 선 그 순간의 STATUS2 바이트에서 ASAT_ANALOG/ASAT_DIGITAL을 함께 확인,
    추가 I2C 트랜잭션 없이 채워준다.
  - `m_i2c.c check_sensor_sanity()`를 정정 — 기존에는 raw count가 고정 임계값
    (`AS7341_SATURATION_THRESHOLD=0xFFFF`)을 넘는지로 소프트웨어에서 포화를
    판정했는데, 이 값은 게인/적분시간이 바뀌면 실제 풀스케일과 안 맞아 정확하지
    않았다(architecture.md §11의 오래된 TODO). 이제는 하드웨어 ASAT 비트를 그대로
    신뢰 — "채널별/게인별 정확한 풀스케일 계산" TODO 자체가 해소됨. `config_app.h`의
    `AS7341_SATURATION_THRESHOLD` 매크로는 더 이상 쓰이지 않아 제거(LOW_SIGNAL
    임계값은 STATUS2에 해당 비트가 없어 기존 소프트웨어 방식 그대로 유지).
  - **빌드 검증 완료**: `west build -b nrf52dk/nrf52832 Application -p always`,
    `version_MCUBOOT: "0.1.26+0"`, 에러 없음, App Flash 74.30%(+16B)/RAM 92.50%
    (변화 없음). **하드웨어 검증 미실시** — 다음 세션에서 강한 광원으로 실제
    포화(ASAT) 유발 시 `MODULE_ERR_SENSOR_SATURATION`이 정확히 판정되는지 확인
    필요(architecture.md AT-04).

