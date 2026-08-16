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

	if ((rxbytes & CC1101_RXBYTES_MASK) < 1)
		goto out_rearm;

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

	if (kfifo_avail(&cc->rx_fifo) >= (unsigned int)(len + 1)) {
		kfifo_in(&cc->rx_fifo, &len, 1);
		kfifo_in(&cc->rx_fifo, payload, len);
		wake_up_interruptible(&cc->rx_wait);
		dev_warn(&cc->spi->dev,
			 "[임시디버그] 패킷 큐에 넣음 (len=%u, rssi_raw=0x%02x, first_byte=0x%02x)\n",
			 len, status[0], payload[0]);
	} else {
		dev_warn(&cc->spi->dev, "RX 소프트웨어 큐 가득 참, 패킷 폐기\n");
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

	if (gpiod_get_value(cc->gdo2))
		cc1101_handle_rx_packet(cc);

	return IRQ_HANDLED;
}

static irqreturn_t cc1101_gdo0_thread(int irq, void *data)
{
	struct cc1101 *cc = data;
	bool level = gpiod_get_value(cc->gdo0);

	mutex_lock(&cc->lock);
	if (cc->state == CC1101_STATE_TX) {
		if (!level) {
			/* falling edge: 송신 완료 */
			cc->state = CC1101_STATE_RX;
			cc1101_enter_rx(cc);
			mutex_unlock(&cc->lock);
			complete(&cc->tx_done);
			return IRQ_HANDLED;
		}
		mutex_unlock(&cc->lock);
		return IRQ_HANDLED;
	}
	mutex_unlock(&cc->lock);

	/* GDO2가 없는 보드에서는 GDO0의 falling edge를 RX 완료로도 사용 */
	if (!cc->gdo2 && !level)
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
		ret = cc1101_enter_rx(cc);
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
					 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
					 IRQF_ONESHOT,
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

	mutex_lock(&cc->lock);
	ret = cc1101_enter_rx(cc);
	mutex_unlock(&cc->lock);
	if (ret) {
		misc_deregister(&cc->miscdev);
		goto err_free_fifo;
	}

	spi_set_drvdata(spi, cc);
	dev_info(dev, "/dev/%s 등록 완료\n", cc->miscdev_name);
	return 0;

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

	mutex_lock(&cc->lock);
	cc1101_enter_idle(cc);
	cc1101_strobe(cc, CC1101_SPWD);
	mutex_unlock(&cc->lock);

	misc_deregister(&cc->miscdev);
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
