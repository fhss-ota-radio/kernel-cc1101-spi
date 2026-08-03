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
