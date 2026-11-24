// ChirpStack v4 기기 프로필 → Codec → JavaScript 에 붙여 넣는 풀이 코드
// 1분 보고 형식 v2(28 B, Core/Inc/report.h) · 옛 v1(24 B) · 횟수 보고(19 B) 를 사람이 읽는 값으로 바꿈
//   포트 1 = 보드(RAK3172, app.h APP_LORA_PORT) 1분 보고 · 포트 8 = Nucleo 벤치(Wio-E5 기본 포트) 1분 보고
//   포트 3 = 보드 횟수 보고(APP_CNT_PORT)
// 서버 참고 구현 tools/report_decode.py 와 같은 눈금 · 같은 거부 규칙(버전과 길이가 짝이 맞아야 함)
// 11/24 v2: 24 · 26 번에 SDP810 차압 1분 평균(0.02 Pa) · DS18B20 온도(0.01 °C), 부호 있는 16비트, 0x8000 = 없음

var FLAGS = ["FLOWING", "VENTING", "DEPLETED", "LEAK_B", "OUT_OF_RANGE", "BATT_WARN", "BATT_QUALITY",
             "ZERO_UNSET", "TIME_UNSYNCED", "RESENT", "CLIPPED", "INCOMPLETE", "BOOT", "FAULT_RESET",
             "BATT_OFF", "BARO_HELD"];
var REPORT_PORTS = [1, 8];
var COUNTS_PORT = 3;

function u16(b, i) { return (b[i] << 8) | b[i + 1]; }
function s16(b, i) { var v = u16(b, i); return v >= 0x8000 ? v - 0x10000 : v; }
function u32(b, i) { return ((b[i] << 24) >>> 0) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]; }
function opt16(b, i, k) { var v = s16(b, i); return v === -32768 ? null : v / k; }

function decodeReport(b) {
  var v2 = b.length === 28 && b[0] === 2;
  var v1 = b.length === 24 && b[0] === 1;
  if (!v2 && !v1) {
    return { errors: ["형식 버전 " + b[0] + " 과 길이 " + b.length + " B 가 안 맞음 (v2 = 28 B · v1 = 24 B)"] };
  }
  var flags = u16(b, 22);
  var names = [];
  for (var k = 0; k < 16; k++) { if (flags & (1 << k)) { names.push(FLAGS[k]); } }
  return { data: {
    version: b[0],
    seq: u16(b, 1),
    unix_s: u32(b, 3),
    total_nm3: u32(b, 7) / 10000,
    flow_nm3h: u16(b, 11) / 1000,
    static_pa: s16(b, 13),            // 정압 (ABP2)
    dp_pa: s16(b, 15) / 20,           // 차압 1분 평균 (HSC)
    gas_temp_c: s16(b, 17) / 10,      // 가스 온도 (DS18B20 이 있으면 그 값)
    patm_pa: u16(b, 19) + 50000,      // 대기압
    batt_v: (b[21] * 10 + 2500) / 1000,
    flags: names,
    sdp810_pa: v2 ? opt16(b, 24, 50) : null,    // SDP810 차압 1분 평균 (v1 · 센서 없음 = null)
    ds18b20_c: v2 ? opt16(b, 26, 100) : null    // DS18B20 가스관 온도 (v1 · 센서 없음 = null)
  } };
}

function decodeCounts(b) {
  if (b.length !== 19 || b[0] !== 1) {
    return { errors: ["횟수 보고 형식 버전 " + b[0] + " · 길이 " + b.length + " B 가 안 맞음 (19 B · 버전 1)"] };
  }
  return { data: {           // 모두 누적값 — 앞 보고와의 차이로 셈
    seq: u16(b, 1),
    cooking: u32(b, 3),
    depletions: u32(b, 7),
    vent_minutes: u32(b, 11),
    leaks: u32(b, 15)
  } };
}

function decodeUplink(input) {
  var b = input.bytes;
  if (REPORT_PORTS.indexOf(input.fPort) >= 0) { return decodeReport(b); }
  if (input.fPort === COUNTS_PORT) { return decodeCounts(b); }
  return { errors: ["모르는 포트 " + input.fPort + " (" + b.length + " B)"] };
}
