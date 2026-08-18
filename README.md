# kernel-cc1101-spi

라즈베리파이용 CC1101 SPI 커널 드라이버 (LKM).

## 핵심 기능
- probe/remove, 레지스터 설정 ioctl 인터페이스
- TX/RX FIFO ↔ read/write syscall 매핑
- GDO0/GDO2 인터럽트 기반 수신 알림
- `/dev/cc1101` 캐릭터 디바이스 노드
- 주소 필터링 비활성화 모드로 1:N 브로드캐스트 지원

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

> 기본 레지스터 값은 433.92MHz / 2-FSK / 38.4kbps 참고 설정입니다. 실제 RF 환경에는
> SmartRF Studio 등으로 재계산한 값을 `cc1101_core.c`의 `cc1101_default_regs[]`에
> 반영하거나 ioctl(`CC1101_IOC_WRITE_REG`, `CC1101_IOC_SET_FREQ`)로 런타임에 조정하세요.
> macOS 등 커널 헤더가 없는 환경에서는 컴파일 검증이 불가능하므로, 실제 라즈베리파이
> 또는 크로스 툴체인 환경에서 `make`로 빌드를 확인해야 합니다.

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

> **⚠️ 싱크워드 주의**: `SYNC1/SYNC0`이 OTA 전용 값 `0x2D/0xD4`로 설정돼 있습니다
> (팀 공용 기본값 `0xD3/0x91`에서 변경 — 팀원들끼리 서로 패킷을 받는 문제가 있었음).
> `gateway-ota`의 `spidevtransport.cpp` 레지스터 배열과 **같은 값이어야** 합니다.
> 자세한 경위는 위 인수 문서 0장 참고.
