import fs from "node:fs/promises";
import path from "node:path";
import { Presentation, PresentationFile } from "@oai/artifact-tool";

const OUT = "C:/Users/kccistc/Desktop/kernel-cc1101-spi/output/device-driver-slides.pptx";
const PREVIEW = "C:/Users/kccistc/Desktop/kernel-cc1101-spi/tmp/device_driver_slides/preview";

const W = 1440;
const H = 810;
const C = {
  ink: "#171412",
  gray: "#6F6A65",
  light: "#D8D2CB",
  pale: "#FBF2ED",
  orange: "#C84308",
  white: "#FFFFFF",
};
const FONT = "Malgun Gothic";

function box(slide, name, x, y, w, h, fill = "none", stroke = "none", width = 0) {
  return slide.shapes.add({
    geometry: "rect",
    name,
    position: { left: x, top: y, width: w, height: h },
    fill,
    line: { style: "solid", fill: stroke, width },
  });
}

function text(slide, name, value, x, y, w, h, size, color = C.ink, bold = false, align = "left") {
  const s = slide.shapes.add({
    geometry: "textbox",
    name,
    position: { left: x, top: y, width: w, height: h },
    fill: "none",
    line: { style: "solid", fill: "none", width: 0 },
  });
  s.text = value;
  s.text.style = {
    fontFamily: FONT,
    fontSize: size,
    color,
    bold,
    alignment: align,
    verticalAlignment: "middle",
  };
  return s;
}

function header(slide, eyebrow, title, page) {
  text(slide, "eyebrow", eyebrow, 96, 48, 620, 32, 17, C.orange, true);
  text(slide, "title", title, 96, 90, 1248, 92, 53, C.ink, true);
  text(slide, "footer", "FHSS-OTA Radio · Device Driver", 96, 758, 520, 24, 14, "#9A948D");
  text(slide, "page", String(page), 1320, 758, 28, 24, 14, "#9A948D", true, "right");
}

function arrow(slide, name, x, y, w = 38) {
  box(slide, `${name}-line`, x, y + 12, w - 12, 3, C.orange);
  const a = slide.shapes.add({
    geometry: "chevron",
    name: `${name}-head`,
    position: { left: x + w - 18, top: y + 4, width: 18, height: 20 },
    fill: C.orange,
    line: { style: "solid", fill: C.orange, width: 0 },
  });
  return a;
}

function addSlide1(p) {
  const s = p.slides.add();
  s.background.fill = C.white;
  header(s, "DEVICE DRIVER · ARCHITECTURE", "사용자 앱과 CC1101을 하나의 장치로 연결", 1);

  const y = 238;
  const bw = 220;
  const bh = 250;
  const xs = [96, 370, 644, 918, 1192];
  const data = [
    ["Gateway App", "open · read · write\nioctl · poll"],
    ["/dev/cc1101", "misc device\n사용자/커널 경계"],
    ["cc1101_main.c", "file_operations · IRQ\nTX completion · RX FIFO"],
    ["cc1101_core.c", "SPI · register · strobe\nIDLE / RX / TX"],
    ["CC1101", "Sub-1GHz RF\nGDO0 · GDO2"],
  ];
  data.forEach((d, i) => {
    const fill = i === 2 ? C.pale : C.white;
    const stroke = i === 2 ? C.orange : C.ink;
    box(s, `arch-box-${i}`, xs[i], y, bw, bh, fill, stroke, i === 2 ? 2 : 1.4);
    text(s, `arch-title-${i}`, d[0], xs[i] + 20, y + 24, bw - 40, 54, 23, i === 2 ? C.orange : C.ink, true);
    text(s, `arch-body-${i}`, d[1], xs[i] + 20, y + 132, bw - 40, 78, 17, C.gray);
    if (i < data.length - 1) arrow(s, `arch-arrow-${i}`, xs[i] + bw + 8, y + 108, 46);
  });

  box(s, "principle-accent", 96, 570, 6, 92, C.orange);
  text(s, "principle-label", "설계 원칙", 126, 568, 150, 32, 18, C.orange, true);
  text(s, "principle-copy", "앱은 패킷만 read/write하고, SPI·FIFO·채널 전환·SYNC는 드라이버가 담당한다.", 126, 604, 1210, 48, 25, C.ink);
}

function addSlide2(p) {
  const s = p.slides.add();
  s.background.fill = C.white;
  header(s, "DEVICE DRIVER · FHSS", "커널이 300 ms마다 채널과 SYNC를 제어한다", 2);

  const x0 = 96;
  const y0 = 250;
  const slotW = 390;
  const gap = 18;
  const channels = ["slot 0 · CH 1", "slot 1 · CH 7", "slot 2 · CH 6"];
  channels.forEach((label, i) => {
    const x = x0 + i * (slotW + gap);
    box(s, `slot-${i}`, x, y0, slotW, 170, i === 0 ? C.pale : C.white, i === 0 ? C.orange : C.light, i === 0 ? 2 : 1.2);
    text(s, `slot-label-${i}`, label, x + 22, y0 + 22, 250, 35, 22, i === 0 ? C.orange : C.ink, true);
    box(s, `guard-${i}`, x + 22, y0 + 83, 32, 42, C.orange);
    text(s, `guard-label-${i}`, "5ms\n전환", x + 61, y0 + 79, 72, 52, 15, C.gray, true);
    box(s, `sync-${i}`, x + 138, y0 + 83, 54, 42, C.ink);
    text(s, `sync-label-${i}`, "SYNC", x + 137, y0 + 129, 60, 25, 13, C.gray, true, "center");
    box(s, `data-${i}`, x + 218, y0 + 83, 145, 42, "#F2EEE9");
    text(s, `data-label-${i}`, "DATA / ACK", x + 218, y0 + 129, 145, 25, 13, C.gray, true, "center");
  });

  text(s, "timeline-caption", "같은 generation + seed + slot → 양쪽이 같은 채널을 독립 계산", 96, 446, 1248, 42, 25, C.ink, true, "center");

  const steps = [
    ["SET_CONFIG", "generation · seed\n채널 수 · 슬롯 시간"],
    ["hop_sequence", "xorshift32로\n채널 permutation 생성"],
    ["hrtimer", "다음 슬롯 경계\n5ms 전에 worker 호출"],
    ["kthread worker", "CHANNR 변경 후\n13-byte SYNC 송신"],
  ];
  const sw = 270;
  steps.forEach((d, i) => {
    const x = 96 + i * 316;
    text(s, `step-no-${i}`, `0${i + 1}`, x, 535, 54, 35, 24, C.orange, true);
    text(s, `step-title-${i}`, d[0], x + 72, 532, 200, 38, 19, C.ink, true);
    text(s, `step-body-${i}`, d[1], x + 72, 578, sw - 72, 66, 16, C.gray);
    if (i < steps.length - 1) arrow(s, `step-arrow-${i}`, x + 270, 570, 42);
  });
}

function addSlide3(p) {
  const s = p.slides.add();
  s.background.fill = C.white;
  header(s, "DEVICE DRIVER · TROUBLESHOOTING", "실기기 오류를 상태·동시성 문제로 좁혔다", 3);

  const cols = [96, 450, 855];
  text(s, "th-problem", "문제", cols[0], 218, 300, 35, 17, C.gray);
  text(s, "th-cause", "원인", cols[1], 218, 350, 35, 17, C.gray);
  text(s, "th-solution", "해결", cols[2], 218, 480, 35, 17, C.gray);
  box(s, "th-line", 96, 260, 1248, 2, C.ink);

  const rows = [
    ["수신이 완전히 멎음", "RX FIFO overflow\nMARCSTATE=0x11", "IDLE → SFRX → SRX\n자동 복구"],
    ["슬롯 1부터 SYNC 거부", "hop_index 의미 불일치", "실제 셔플 채널의\n0-based index로 통일"],
    ["초기 동기 획득 실패", "같은 slot 0을\n20ms 간격으로 3회 송신", "실제 슬롯마다\nSYNC 한 번 송신"],
    ["OTA_DATA write 실패", "사용자 DATA와 내부\nSYNC TX 충돌", "tx_lock으로 송신 직렬화"],
  ];
  const rowY = [282, 380, 478, 576];
  rows.forEach((r, i) => {
    if (i % 2 === 1) box(s, `row-bg-${i}`, 76, rowY[i] - 10, 1288, 90, C.pale);
    text(s, `problem-${i}`, r[0], cols[0], rowY[i], 320, 60, 19, C.ink, true);
    text(s, `cause-${i}`, r[1], cols[1], rowY[i], 350, 60, 17, C.gray);
    text(s, `solution-${i}`, r[2], cols[2], rowY[i], 440, 60, 17, C.orange, true);
  });

  box(s, "result-accent", 96, 688, 6, 42, C.orange);
  text(s, "result-label", "실기기 확인", 120, 684, 130, 40, 17, C.orange, true);
  text(s, "result-copy", "SYNC_ACQUIRED → TRACKING · 시간 오차 -22 ~ +18 us · 채널 1→7→6→5→3→2→8→4", 260, 684, 1060, 40, 19, C.ink, true);
}

async function writeBlob(file, blob) {
  await fs.writeFile(file, new Uint8Array(await blob.arrayBuffer()));
}

async function main() {
  await fs.mkdir(PREVIEW, { recursive: true });
  await fs.mkdir(path.dirname(OUT), { recursive: true });
  const p = Presentation.create({ slideSize: { width: W, height: H } });
  addSlide1(p);
  addSlide2(p);
  addSlide3(p);

  for (const [i, slide] of p.slides.items.entries()) {
    await writeBlob(path.join(PREVIEW, `slide-${i + 1}.png`), await p.export({ slide, format: "png", scale: 1 }));
    const layout = await slide.export({ format: "layout" });
    await fs.writeFile(path.join(PREVIEW, `slide-${i + 1}.layout.json`), await layout.text());
  }
  await writeBlob(path.join(PREVIEW, "montage.webp"), await p.export({ format: "webp", montage: true, scale: 1 }));
  const pptx = await PresentationFile.exportPptx(p);
  await pptx.save(OUT);
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
