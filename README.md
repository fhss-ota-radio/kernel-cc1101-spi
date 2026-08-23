# kernel-cc1101-spi

라즈베리파이용 CC1101 SPI 커널 드라이버 (LKM).

## 핵심 기능
- probe/remove, 레지스터 설정 ioctl 인터페이스
- TX/RX FIFO ↔ read/write syscall 매핑
- GDO0/GDO2 인터럽트 기반 수신 알림
- `/dev/cc1101` 캐릭터 디바이스 노드
- 주소 필터링 비활성화 모드로 1:N 브로드캐스트 지원
- 커널 hrtimer/kthread 기반 FHSS 자동 채널 전환
- MASTER SYNC 송신, SLAVE 동기 획득·시간 보정·상실 후 랑데부 재탐색

## 담당
팀원3, 4

## 파일 구성
- `cc1101.h` - 레지스터/스트로브 상수, 드라이버 내부 struct 정의
- `cc1101_ioctl.h` - `/dev/cc1101` 유저 스페이스 ioctl UAPI
- `cc1101_core.c` - SPI 레지스터 접근, 리셋, 기본 설정 테이블, 주파수/주소필터 계산
- `cc1101_main.c` - spi_driver probe/remove, GDO0/GDO2 인터럽트, 캐릭터 디바이스 fops
- `dts/cc1101-overlay.dts` - 라즈베리파이 SPI0 CE0 + GDO0/GDO2 GPIO 디바이스트리 오버레이
- `examples/cc1101_test.c` - read/write/ioctl 사용 예제 겸 테스트 도구

## 빌드 및 적재 (라즈베리파이)
```sh
# 1. 디바이스트리 오버레이 빌드/설치
dtc -@ -I dts -O dtb -o cc1101.dtbo dts/cc1101-overlay.dts
sudo cp cc1101.dtbo /boot/overlays/
echo "dtparam=spi=on" | sudo tee -a /boot/config.txt
echo "dtoverlay=cc1101" | sudo tee -a /boot/config.txt
sudo reboot

# 2. 커널 모듈 빌드/적재
make
sudo insmod cc1101.ko
dmesg | tail                     # "/dev/cc1101 등록 완료" 확인

# 3. 테스트
gcc -o examples/cc1101_test examples/cc1101_test.c
./examples/cc1101_test status
./examples/cc1101_test rx &
./examples/cc1101_test tx "hello"
```

## 라즈베리파이 2대 링크 테스트

`cc1101_link_test`는 한쪽 MASTER가 PING을 보내고 다른 쪽 SLAVE가 PONG으로
응답하게 해서 양방향 링크를 확인합니다. 먼저 고정 채널로 하드웨어와 기본 RF
설정을 검증하고, 성공한 뒤 FHSS 모드로 넘어가면 문제 범위를 쉽게 나눌 수 있습니다.

```sh
gcc -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
    -o examples/cc1101_link_test examples/cc1101_link_test.c

# 1단계: 고정 채널 0 (Pi B를 먼저 실행)
sudo ./examples/cc1101_link_test fixed slave
sudo ./examples/cc1101_link_test fixed master

# 2단계: FHSS 채널 1~4 (Pi B를 먼저 실행)
sudo ./examples/cc1101_link_test fhss slave
sudo ./examples/cc1101_link_test fhss master
```

두 FHSS 프로세스의 `seed`, `generation`, `channels` 값은 반드시 같아야 합니다.
기본 채널 1~4는 현재 433.92MHz/약 200kHz 간격 설정에서 국내 433MHz 대역
상한을 넘지 않도록 제한한 테스트 범위입니다.

> 기본 레지스터 값은 433.92MHz / 2-FSK / 38.4kbps 참고 설정입니다. 실제 RF 환경에는
> SmartRF Studio 등으로 재계산한 값을 `cc1101_core.c`의 `cc1101_default_regs[]`에
> 반영하거나 ioctl(`CC1101_IOC_WRITE_REG`, `CC1101_IOC_SET_FREQ`)로 런타임에 조정하세요.
> macOS 등 커널 헤더가 없는 환경에서는 컴파일 검증이 불가능하므로, 실제 라즈베리파이
> 또는 크로스 툴체인 환경에서 `make`로 빌드를 확인해야 합니다.

## FHSS 동작 방식

사용자 앱은 매 슬롯마다 `SET_CHANNEL`을 호출하지 않습니다. 세션을 시작할 때
`CC1101_IOC_FHSS_SET_CONFIG`와 `CC1101_IOC_FHSS_START`만 호출한 뒤에는 기존처럼
`read()`/`write()`를 사용합니다.

1. MASTER와 SLAVE에 같은 `generation`, seed, 채널 범위, 슬롯 시간을 설정합니다.
2. SLAVE를 먼저 시작하면 랑데부 채널에서 SYNC를 기다립니다.
3. MASTER를 시작하면 랑데부 채널에서 SYNC를 세 번 보내고 자동 호핑합니다.
4. SLAVE는 유효한 SYNC 세 개를 받은 뒤 MASTER의 슬롯 번호에 맞춰 호핑합니다.
5. SYNC를 다섯 슬롯 동안 놓치면 랑데부 채널로 돌아가 다시 찾습니다.
6. `CC1101_IOC_FHSS_STOP`을 호출하면 호핑을 멈추고 OTA용 `reserved_channel`로
   자동 복귀합니다.

현재 공용 프로토콜과 맞는 대표 설정은 seed `0x46485353`, 슬롯 `300000us`,
전환 guard `5000us`, 알고리즘 버전 1입니다. 실제 사용할 채널 범위는 안테나
대역과 지역 전파 규정을 확인한 뒤 정해야 합니다.

## 문서

- [`docs/troubleshooting-cc1101.md`](docs/troubleshooting-cc1101.md) — **증상별 트러블슈팅
  가이드.** 안 될 때 여기부터 보세요 (안테나/전원/배선/SPI/설정/큐/인터럽트 폭주,
  진단 기법 포함)
- [`docs/pi-bringup-guide.md`](docs/pi-bringup-guide.md) — 커널 모듈 빌드/적재 상세 가이드
  (커스텀 커널·크로스컴파일 대응)
- [`docs/driver-changes-handoff-2026-08-17.md`](docs/driver-changes-handoff-2026-08-17.md) —
  2026-08-16~17 변경 내역과 담당자 리뷰 요청 사항
- `tools/cc1101_diag.c` — 칩 레지스터/`MARCSTATE`를 직접 읽는 진단 도구.
  문제 생기면 추측하기 전에 먼저 돌려보세요

> **싱크워드 주의**: 현재 `SYNC1/SYNC0`은 ESP32와 같은 `0xD3/0x91`입니다.
> `firmware-esp32`와 `gateway-ota`의 우회 SPI 설정도 반드시 같은 값이어야 하며,
> 한쪽만 바꾸면 CC1101 하드웨어 단계에서 서로의 패킷을 받지 못합니다.
