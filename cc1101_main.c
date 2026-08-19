// SPDX-License-Identifier: GPL-2.0
/*
 * cc1101_main.c - TI CC1101 SPI 캐릭터 디바이스 드라이버
 *
 * /dev/cc1101 을 통해 하나의 CC1101 트랜시버를 노출한다.
 *   write() - 페이로드 1개를 TX FIFO에 적재하고 STX로 송신, 완료까지 블로킹
 *   read()  - 수신된 패킷 1개를 반환, 수신할 때까지 블로킹 (O_NONBLOCK 지원)
 *   ioctl() - 레지스터/스트로브 접근, 주파수/채널/주소/필터/송신출력 설정
 *
 * RX는 GDO0/GDO2 GPIO 인터럽트로 알림받는다:
 *   - GDO2가 DT에 배선되어 있으면 GDO2 rising edge = "CRC OK 패킷 수신" 전용 알림
 *   - GDO0은 항상 필수이며 TX 완료 알림(및 GDO2 미배선 시 RX 완료 알림)을 겸한다
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/kfifo.h>
#include <linux/string.h>
#include <linux/err.h>

#include "cc1101.h"
#include "cc1101_fhss.h"
#include "cc1101_ioctl.h"

static int cc1101_handle_rx_packet(struct cc1101 *cc)
{
	u8 rxbytes, len, status[2];
	u8 payload[CC1101_MAX_PACKET_LEN];
	int ret;

	mutex_lock(&cc->lock);

	ret = cc1101_read_status_reg(cc, CC1101_RXBYTES, &rxbytes);
	if (ret)
		goto out_unlock;

	if (rxbytes & CC1101_RXBYTES_OVERFLOW) {
		dev_warn(&cc->spi->dev, "RX FIFO overflow, flush\n");
		cc1101_enter_idle(cc);
		cc1101_strobe(cc, CC1101_SFRX);
		cc1101_enter_rx(cc);
		goto out_unlock;
	}

	if ((rxbytes & CC1101_RXBYTES_MASK) < 1) {
		/* [2026-08-16 임시 디버그] GDO2 인터럽트는 울렸는데(카운트 증가),
		 * 정작 SPI로 RXBYTES를 읽어보니 0인 경우 — 인터럽트 발생 시점과
		 * 실제 SPI 읽기 시점 사이의 타이밍/레이스 의심 지점. */
		dev_warn(&cc->spi->dev,
			 "[임시디버그] GDO2 울렸는데 RXBYTES=0x%02x (빈 FIFO)\n", rxbytes);
		goto out_rearm;
	}

	ret = cc1101_read_burst(cc, CC1101_RXFIFO, &len, 1);
	if (ret)
		goto out_unlock;

	if (len == 0 || len > CC1101_MAX_PACKET_LEN) {
		dev_warn(&cc->spi->dev, "잘못된 패킷 길이 %u, flush\n", len);
		cc1101_enter_idle(cc);
		cc1101_strobe(cc, CC1101_SFRX);
		cc1101_enter_rx(cc);
		goto out_unlock;
	}

	ret = cc1101_read_burst(cc, CC1101_RXFIFO, payload, len);
	if (ret)
		goto out_unlock;

	/* PKTCTRL1.APPEND_STATUS=1 이므로 페이로드 뒤에 RSSI, LQI+CRC_OK가 붙는다 */
	ret = cc1101_read_burst(cc, CC1101_RXFIFO, status, sizeof(status));
	if (ret)
		goto out_unlock;

	/* [2026-08-16 임시 디버그] dynamic_debug가 이 커널엔 안 켜져 있어서
	 * dev_dbg는 dmesg에 절대 안 보임 -> 원인 파악을 위해 잠깐 dev_warn으로
	 * 올려서 항상 보이게 함. 원인 확인되면 dev_dbg로 되돌릴 것. */
	if (!(status[1] & CC1101_LQI_CRC_OK)) {
		dev_warn(&cc->spi->dev,
			 "[임시디버그] CRC 오류, 패킷 폐기 (len=%u, rssi_raw=0x%02x, status=0x%02x)\n",
			 len, status[0], status[1]);
		goto out_rearm;
	}

	/* [버그 수정 2026-08-16] 큐가 가득 차면 "새 패킷"이 아니라 "오래된
	 * 패킷"부터 버린다.
	 *
	 * 이전 코드는 자리가 없으면 방금 받은 패킷을 그냥 버렸다. 그러면 한 번
	 * 큐가 차는 순간부터 영원히 새 패킷이 못 들어와서, 유저 프로그램이
	 * 큐를 다 비워주기 전까지 수신이 완전히 마비된다(실기기에서 실제로
	 * 이 상태에 빠짐 — 같은 대역을 쓰는 다른 팀 패킷이 큐를 채워버렸고,
	 * 그 뒤로 우리 OTA 패킷이 하나도 안 올라옴).
	 *
	 * 통신에서는 "오래된 데이터"가 "새 데이터"보다 가치가 낮다. 어차피
	 * 놓칠 거라면 지나간 것을 버리는 쪽이 맞다.
	 */
	while (kfifo_avail(&cc->rx_fifo) < (unsigned int)(len + 1)) {
		u8 drop_len;
		u8 scratch[CC1101_MAX_PACKET_LEN];

		if (kfifo_out(&cc->rx_fifo, &drop_len, 1) != 1)
			break;			/* 큐가 비었는데도 자리가 없으면 포기 */
		if (drop_len > CC1101_MAX_PACKET_LEN) {
			/* 큐 내용이 깨졌다는 뜻 — 통째로 비우고 새로 시작 */
			kfifo_reset(&cc->rx_fifo);
			dev_warn(&cc->spi->dev,
				 "RX 큐 내용 손상(len=%u), 큐 초기화\n", drop_len);
			break;
		}
		if (kfifo_out(&cc->rx_fifo, scratch, drop_len) != drop_len) {
			/* 길이바이트는 있었는데 본문이 모자람 = 큐 내용 깨짐 */
			kfifo_reset(&cc->rx_fifo);
			dev_warn(&cc->spi->dev, "RX 큐 내용 불일치, 큐 초기화\n");
			break;
		}
		dev_warn_ratelimited(&cc->spi->dev,
				      "RX 큐 가득 참 — 오래된 패킷 1개 버림\n");
	}

	if (kfifo_avail(&cc->rx_fifo) >= (unsigned int)(len + 1)) {
		kfifo_in(&cc->rx_fifo, &len, 1);
		kfifo_in(&cc->rx_fifo, payload, len);
		wake_up_interruptible(&cc->rx_wait);
		dev_warn(&cc->spi->dev,
			 "[임시디버그] 패킷 큐에 넣음 (len=%u, rssi_raw=0x%02x, first_byte=0x%02x)\n",
			 len, status[0], payload[0]);
	} else {
		dev_warn_ratelimited(&cc->spi->dev,
				      "RX 큐 확보 실패, 패킷 폐기 (len=%u)\n", len);
	}

out_rearm:
	/* MCSM1.RXOFF_MODE=RX 로 보통 자동 유지되지만, 방어적으로 재진입 */
	if (cc->state != CC1101_STATE_TX)
		cc1101_enter_rx(cc);
out_unlock:
	mutex_unlock(&cc->lock);
	return ret;
}

static irqreturn_t cc1101_gdo2_thread(int irq, void *data)
{
	struct cc1101 *cc = data;

	/* rising-edge IRQ 자체가 이벤트이므로 스레드 실행 시점의 레벨을
	 * 다시 읽지 않는다. 짧은 펄스라면 그 사이 low로 바뀔 수 있다. */
	cc1101_handle_rx_packet(cc);

	return IRQ_HANDLED;
}

static irqreturn_t cc1101_gdo0_thread(int irq, void *data)
{
	struct cc1101 *cc = data;

	mutex_lock(&cc->lock);
	if (cc->state == CC1101_STATE_TX) {
		/* GDO0은 falling edge만 등록한다. IOCFG0=0x06에서 이 에지는
		 * 송신 또는 수신 패킷의 끝을 의미한다.
		 *
		 * 송신 완료 -> 명시적으로 RX 재진입
			 *
			 * [되돌림 2026-08-16] 한때 여기서 cc1101_enter_rx() 호출을
			 * 제거했다가 되돌렸다. 경위를 남긴다.
			 *
			 * 제거했던 이유(가설): GDO0(IOCFG0=0x06)의 falling edge는
			 * "패킷 끝"이지만 마지막 몇 바이트가 아직 안테나로 나가는
			 * 중일 수 있어, 그 순간 SRX를 강제하면 칩이 애매한 상태에
			 * 빠질 수 있다. MCSM1=0x3F(TXOFF_MODE=11)가 이미 "송신 끝나면
			 * 자동 RX 복귀"이므로 수동 SRX는 불필요하다고 판단했다.
			 * gateway-ota의 SpidevTransport에서 유사한 증상을 같은 방식으로
			 * 고친 전례도 있었다(design-notes-gateway-ota-es.md 17절).
			 *
			 * 되돌린 이유(실측): 이 변경을 올린 뒤 송신측 pi24의
			 * /proc/interrupts에서 cc1101-gdo0 카운트가 7,700만 회를
			 * 넘겼다 — 인터럽트 폭주(IRQ storm). 폭주는 송신을 수행한
			 * 쪽에서만 발생했고 수신 전용이던 pi06은 정상(gdo0=6)이었다.
			 * 즉 SRX를 생략하면 송신 후 칩이 안정된 RX 상태로 수습되지
			 * 않고 GDO0이 계속 토글하는 상태에 남는다.
			 *
			 * 결론: MCSM1의 자동 복귀만 믿으면 안 되고, 명시적 SRX로
			 * 상태를 확정시켜야 한다. 원래 코드가 맞았다.
		 */
		cc->tx_result = cc1101_enter_rx_recover(cc);
		mutex_unlock(&cc->lock);
		complete(&cc->tx_done);
		return IRQ_HANDLED;
	}
	mutex_unlock(&cc->lock);

	/* RX 완료는 GDO0(IOCFG0=0x06)의 falling edge를 사용한다. 일부 보드에서
	 * DT에 GDO2가 선언되어도 실제 GDO2 IRQ가 발생하지 않아 RX FIFO가 영원히
	 * drain되지 않았다. TX 상태는 위에서 처리하고 return하므로 여기서는 RX
	 * 패킷 종료만 처리한다. */
	cc1101_handle_rx_packet(cc);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------
 * 캐릭터 디바이스 fops
 * ------------------------------------------------------------------- */

static int cc1101_open(struct inode *inode, struct file *filp)
{
	struct cc1101 *cc = container_of(filp->private_data,
					  struct cc1101, miscdev);

	if (atomic_cmpxchg(&cc->open_count, 0, 1) != 0)
		return -EBUSY;

	/* [버그 수정 2026-08-16] 열 때 RX 큐를 비운다.
	 *
	 * 드라이버는 probe()에서 RX에 들어간 순간부터 계속 수신해서 kfifo에
	 * 쌓는다. 그런데 그걸 꺼내가는 유저 프로그램은 한참 뒤에야 붙는다.
	 * 그 사이에 쌓인 데이터는 이미 지나간 남의 패킷이라 쓸모가 없는데,
	 * 큐를 차지한 채로 남아서 정작 필요한 패킷이 들어올 자리를 막는다.
	 *
	 * 실기기 확인(2026-08-16): 같은 433.92MHz/같은 싱크워드를 쓰는 다른
	 * 팀 장비들의 패킷("FHSS"=0x46485353 로 시작하는 것 등)이 계속 잡혀서
	 * kfifo(512byte)가 가득 찬 상태로 유지됐고, 그 결과 우리 OTA 패킷은
	 * 도착해도 전부 "RX 소프트웨어 큐 가득 참"으로 폐기됐다.
	 */
	mutex_lock(&cc->lock);
	kfifo_reset(&cc->rx_fifo);
	mutex_unlock(&cc->lock);

	filp->private_data = cc;
	return 0;
}

static int cc1101_release(struct inode *inode, struct file *filp)
{
	struct cc1101 *cc = filp->private_data;

	atomic_set(&cc->open_count, 0);
	return 0;
}

static ssize_t cc1101_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct cc1101 *cc = filp->private_data;
	u8 len;
	u8 kbuf[CC1101_MAX_PACKET_LEN];
	int ret;

	if (kfifo_is_empty(&cc->rx_fifo)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(cc->rx_wait,
						!kfifo_is_empty(&cc->rx_fifo));
		if (ret)
			return ret;
	}

	if (kfifo_out(&cc->rx_fifo, &len, 1) != 1)
		return -EIO;
	if (WARN_ON(kfifo_out(&cc->rx_fifo, kbuf, len) != len))
		return -EIO;

	if (len > count)
		len = count;	/* 사용자 버퍼가 작으면 잘라서 반환 */

	if (copy_to_user(buf, kbuf, len))
		return -EFAULT;

	return len;
}

static ssize_t cc1101_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct cc1101 *cc = filp->private_data;
	u8 kbuf[CC1101_MAX_PACKET_LEN];
	u8 txbuf[CC1101_MAX_PACKET_LEN + 1];
	long timeout;
	int ret;

	if (count == 0)
		return 0;
	if (count > CC1101_MAX_PACKET_LEN)
		return -EMSGSIZE;
	if (copy_from_user(kbuf, buf, count))//사용자 프로그램의 내용 복사해오기.
		return -EFAULT;

	ret = mutex_lock_interruptible(&cc->lock); //mutex lock 잡기.
	if (ret)
		return ret;

	if (cc->state == CC1101_STATE_TX) {//이미 송신중인지 검사.
		mutex_unlock(&cc->lock);
		return -EBUSY;
	}

	reinit_completion(&cc->tx_done);//tx 완료상태 초기화.
	cc->tx_result = 0;

	cc1101_enter_idle(cc);
	cc1101_strobe(cc, CC1101_SFTX);	/* 이전 잔여 데이터 flush (IDLE 상태 필수) */

	txbuf[0] = (u8)count;
	memcpy(&txbuf[1], kbuf, count);
	ret = cc1101_write_burst(cc, CC1101_TXFIFO, txbuf, count + 1);//TXFIFO에 데이터 넣기.
	if (ret) {
		cc1101_enter_rx(cc); //cc 1101 idle 변경.
		mutex_unlock(&cc->lock);
		return ret;
	}

	cc->state = CC1101_STATE_TX;//드라이버 상태를 TX로 변경.
	ret = cc1101_strobe(cc, CC1101_STX);//idle 상태 strobe 명령. 
	if (ret) {
		cc1101_enter_idle(cc);
		cc1101_strobe(cc, CC1101_SFTX);
		cc1101_enter_rx(cc);
	}
	mutex_unlock(&cc->lock); //SPI 작업 끝났으니 lock 해제.
	if (ret)
		return ret;

	timeout = wait_for_completion_timeout(&cc->tx_done,
					       msecs_to_jiffies(CC1101_TX_TIMEOUT_MS));
						   //GDO0에서 TX 완료 신호 올때까지 대기. timeout 설정.
	if (!timeout) {
		dev_warn(&cc->spi->dev, "TX 타임아웃\n");
		mutex_lock(&cc->lock);
		cc1101_enter_idle(cc);
		cc1101_strobe(cc, CC1101_SFTX);//TX FIFO 비우기.
		cc1101_enter_rx(cc);
		mutex_unlock(&cc->lock);
		return -ETIMEDOUT;
	}

	mutex_lock(&cc->lock);
	ret = cc->tx_result;
	if (ret) {
		cc1101_enter_idle(cc);
		cc1101_strobe(cc, CC1101_SFTX);
		cc1101_enter_rx_recover(cc);
	}
	mutex_unlock(&cc->lock);
	if (ret)
		return ret;

	return count;
}

static __poll_t cc1101_poll(struct file *filp, poll_table *wait)
{
	struct cc1101 *cc = filp->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(filp, &cc->rx_wait, wait);
	if (!kfifo_is_empty(&cc->rx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static long cc1101_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct cc1101 *cc = filp->private_data;
	void __user *argp = (void __user *)arg;
	int ret = 0;

	switch (cmd) {
	case CC1101_IOC_RESET: {
		mutex_lock(&cc->lock);
		ret = cc1101_hw_reset(cc);
		if (!ret)
			ret = cc1101_load_default_config(cc);
		if (!ret)
			ret = cc1101_enter_rx(cc);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_STROBE: {
		u8 strobe;

		if (copy_from_user(&strobe, argp, sizeof(strobe)))
			return -EFAULT;
		if (strobe < CC1101_SRES || strobe > CC1101_SNOP)
			return -EINVAL;

		mutex_lock(&cc->lock);
		ret = cc1101_strobe(cc, strobe);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_READ_REG: {
		struct cc1101_reg_io io;

		if (copy_from_user(&io, argp, sizeof(io)))
			return -EFAULT;
		if (io.addr > CC1101_TEST0 && io.addr != CC1101_PATABLE)
			return -EINVAL;

		mutex_lock(&cc->lock);
		ret = cc1101_read_reg(cc, io.addr, &io.value);
		mutex_unlock(&cc->lock);
		if (ret)
			break;

		if (copy_to_user(argp, &io, sizeof(io)))
			return -EFAULT;
		break;
	}
	case CC1101_IOC_WRITE_REG: {
		struct cc1101_reg_io io;

		if (copy_from_user(&io, argp, sizeof(io)))
			return -EFAULT;
		if (io.addr > CC1101_TEST0 && io.addr != CC1101_PATABLE)
			return -EINVAL;

		mutex_lock(&cc->lock);
		ret = cc1101_write_reg(cc, io.addr, io.value);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_SET_FREQ: {
		struct cc1101_freq_cfg cfg;

		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;

		mutex_lock(&cc->lock);
		ret = cc1101_set_freq_hz(cc, cfg.freq_hz);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_SET_CHANNEL: {
		u8 ch;

		if (copy_from_user(&ch, argp, sizeof(ch)))
			return -EFAULT;

		mutex_lock(&cc->lock);
		ret = cc1101_write_reg(cc, CC1101_CHANNR, ch);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_SET_ADDR: {
		u8 addr;

		if (copy_from_user(&addr, argp, sizeof(addr)))
			return -EFAULT;

		mutex_lock(&cc->lock);
		ret = cc1101_write_reg(cc, CC1101_ADDR, addr);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_SET_ADDR_FILTER: {
		u8 mode;

		if (copy_from_user(&mode, argp, sizeof(mode)))
			return -EFAULT;

		mutex_lock(&cc->lock);
		ret = cc1101_set_addr_filter(cc, mode);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_SET_PA_POWER: {
		u8 power;

		if (copy_from_user(&power, argp, sizeof(power)))
			return -EFAULT;

		mutex_lock(&cc->lock);
		ret = cc1101_write_burst(cc, CC1101_PATABLE, &power, 1);
		mutex_unlock(&cc->lock);
		break;
	}
	case CC1101_IOC_GET_STATUS: {
		struct cc1101_status st = { 0 };
		u8 lqi;

		mutex_lock(&cc->lock);
		ret = cc1101_read_rssi_dbm(cc, &st.rssi_dbm);
		if (!ret)
			ret = cc1101_read_status_reg(cc, CC1101_LQI, &lqi);
		if (!ret)
			ret = cc1101_read_status_reg(cc, CC1101_MARCSTATE,
						      &st.marc_state);
		mutex_unlock(&cc->lock);
		if (ret)
			break;

		st.lqi = lqi & CC1101_LQI_MASK;
		st.crc_ok = !!(lqi & CC1101_LQI_CRC_OK);

		if (copy_to_user(argp, &st, sizeof(st)))
			return -EFAULT;
		break;
	}
	case CC1101_IOC_SET_RX:
		mutex_lock(&cc->lock);
		ret = cc1101_enter_rx_recover(cc);
		mutex_unlock(&cc->lock);
		break;
	case CC1101_IOC_SET_IDLE:
		mutex_lock(&cc->lock);
		ret = cc1101_enter_idle(cc);
		mutex_unlock(&cc->lock);
		break;
	case CC1101_IOC_FLUSH_RX:
		mutex_lock(&cc->lock);
		ret = cc1101_enter_idle(cc);
		if (!ret)
			ret = cc1101_strobe(cc, CC1101_SFRX);
		if (!ret)
			ret = cc1101_enter_rx(cc);
		kfifo_reset(&cc->rx_fifo);
		mutex_unlock(&cc->lock);
		break;
	case CC1101_IOC_FLUSH_TX:
		mutex_lock(&cc->lock);
		ret = cc1101_enter_idle(cc);
		if (!ret)
			ret = cc1101_strobe(cc, CC1101_SFTX);
		if (!ret)
			ret = cc1101_enter_rx(cc);
		mutex_unlock(&cc->lock);
		break;
	case CC1101_IOC_FHSS_SET_CONFIG: {
		struct cc1101_fhss_config config;

		if (copy_from_user(&config, argp, sizeof(config)))
			return -EFAULT;
		ret = cc1101_fhss_set_config(cc, &config);
		break;
	}
	case CC1101_IOC_FHSS_START: {
		u8 role;

		if (copy_from_user(&role, argp, sizeof(role)))
			return -EFAULT;
		ret = cc1101_fhss_start(cc, role);
		break;
	}

	case CC1101_IOC_FHSS_STOP:
		ret = cc1101_fhss_stop(cc);
		break;

	case CC1101_IOC_FHSS_GET_STATUS: {
		struct cc1101_fhss_status status;

		cc1101_fhss_get_status(cc, &status);
		if (copy_to_user(argp, &status, sizeof(status)))
			return -EFAULT;
		break;
	}
	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations cc1101_fops = {
	.owner		= THIS_MODULE,
	.open		= cc1101_open,
	.release	= cc1101_release,
	.read		= cc1101_read,
	.write		= cc1101_write,
	.poll		= cc1101_poll,
	.unlocked_ioctl	= cc1101_ioctl,
};

/* ---------------------------------------------------------------------
 * SPI probe/remove
 * ------------------------------------------------------------------- */

static atomic_t cc1101_instance_id = ATOMIC_INIT(0);

static int cc1101_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct cc1101 *cc;
	int ret, id;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	cc->spi = spi;
	mutex_init(&cc->lock);
	init_completion(&cc->tx_done);
	init_waitqueue_head(&cc->rx_wait);
	atomic_set(&cc->open_count, 0);
	cc->state = CC1101_STATE_IDLE;

	ret = kfifo_alloc(&cc->rx_fifo, CC1101_RX_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;

	spi->mode = SPI_MODE_0;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = 4000000;	/* CC1101 SPI 안전 상한 내 값 */
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "spi_setup 실패: %d\n", ret);
		goto err_free_fifo;
	}

	cc->gdo0 = devm_gpiod_get(dev, "gdo0", GPIOD_IN);
	if (IS_ERR(cc->gdo0)) {
		ret = PTR_ERR(cc->gdo0);
		dev_err(dev, "gdo0 GPIO 요청 실패: %d\n", ret);
		goto err_free_fifo;
	}

	cc->gdo2 = devm_gpiod_get_optional(dev, "gdo2", GPIOD_IN);
	if (IS_ERR(cc->gdo2)) {
		ret = PTR_ERR(cc->gdo2);
		dev_err(dev, "gdo2 GPIO 요청 실패: %d\n", ret);
		goto err_free_fifo;
	}

	ret = cc1101_hw_reset(cc);
	if (ret)
		goto err_free_fifo;

	ret = cc1101_load_default_config(cc);
	if (ret)
		goto err_free_fifo;

	cc->irq_gdo0 = gpiod_to_irq(cc->gdo0);
	if (cc->irq_gdo0 < 0) {
		ret = cc->irq_gdo0;
		goto err_free_fifo;
	}

	ret = devm_request_threaded_irq(dev, cc->irq_gdo0, NULL,
					 cc1101_gdo0_thread,
					 IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					 "cc1101-gdo0", cc);
	if (ret) {
		dev_err(dev, "gdo0 IRQ 요청 실패: %d\n", ret);
		goto err_free_fifo;
	}

	if (cc->gdo2) {
		cc->irq_gdo2 = gpiod_to_irq(cc->gdo2);
		if (cc->irq_gdo2 < 0) {
			ret = cc->irq_gdo2;
			goto err_free_fifo;
		}

		ret = devm_request_threaded_irq(dev, cc->irq_gdo2, NULL,
						 cc1101_gdo2_thread,
						 IRQF_TRIGGER_RISING | IRQF_ONESHOT,
						 "cc1101-gdo2", cc);
		if (ret) {
			dev_err(dev, "gdo2 IRQ 요청 실패: %d\n", ret);
			goto err_free_fifo;
		}
	}

	id = atomic_fetch_inc(&cc1101_instance_id);
	if (id == 0)
		strscpy(cc->miscdev_name, "cc1101", sizeof(cc->miscdev_name));
	else
		snprintf(cc->miscdev_name, sizeof(cc->miscdev_name), "cc1101-%d", id);

	cc->miscdev.minor = MISC_DYNAMIC_MINOR;
	cc->miscdev.name = cc->miscdev_name;
	cc->miscdev.fops = &cc1101_fops;
	cc->miscdev.parent = dev;

	ret = misc_register(&cc->miscdev);
	if (ret) {
		dev_err(dev, "misc_register 실패: %d\n", ret);
		goto err_free_fifo;
	}
	spi_set_drvdata(spi, cc);

	mutex_lock(&cc->lock);
	ret = cc1101_enter_rx(cc);
	mutex_unlock(&cc->lock);
	if (ret) {
		misc_deregister(&cc->miscdev);
		goto err_free_fifo;
	}

	ret = cc1101_fhss_init(cc);
	if (ret)
		goto err_deregister_misc;

	dev_info(dev, "/dev/%s 등록 완료\n", cc->miscdev_name);
	return 0;

err_deregister_misc:
	misc_deregister(&cc->miscdev);
err_free_fifo:
	kfifo_free(&cc->rx_fifo);
	return ret;
}

/* spi_driver.remove가 void를 반환하는 최신 API (커널 6.5+) 기준. 이전 커널이면
 * `int cc1101_remove(...)` 로 바꾸고 마지막에 `return 0;`을 추가해야 한다.
 *
 * [2026-08-16] 실제로 타겟 커널(6.12.92-v7l+)이 6.5+ API라 위 주석대로
 * void로 고쳐야 컴파일됨 (int로 두면 "incompatible pointer type" 에러).
 * 라즈베리파이 실기기에서 크로스컴파일 검증 완료.
 */
static void cc1101_remove(struct spi_device *spi)
{
	struct cc1101 *cc = spi_get_drvdata(spi);

	cc1101_fhss_destroy(cc);
	misc_deregister(&cc->miscdev);

	mutex_lock(&cc->lock);
	cc1101_enter_idle(cc);
	cc1101_strobe(cc, CC1101_SPWD);
	mutex_unlock(&cc->lock);

	kfifo_free(&cc->rx_fifo);
}

static const struct of_device_id cc1101_of_match[] = {
	{ .compatible = "ti,cc1101" },
	{ }
};
MODULE_DEVICE_TABLE(of, cc1101_of_match);

static const struct spi_device_id cc1101_spi_id[] = {
	{ "cc1101", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, cc1101_spi_id);

static struct spi_driver cc1101_spi_driver = {
	.driver = {
		.name = "cc1101",
		.of_match_table = cc1101_of_match,
	},
	.probe = cc1101_probe,
	.remove = cc1101_remove,
	.id_table = cc1101_spi_id,
};
module_spi_driver(cc1101_spi_driver);

MODULE_AUTHOR("kernel-cc1101-spi contributors");
MODULE_DESCRIPTION("TI CC1101 sub-1GHz 트랜시버 SPI 캐릭터 디바이스 드라이버");
MODULE_LICENSE("GPL");
