// ChirpStack v4 기기 프로필 → Codec → JavaScript 에 붙여 넣는 풀이 코드
// 1분 보고 형식 v2(28 B, Core/Inc/report.h) · 옛 v1(24 B) · 횟수 보고(19 B) 를 사람이 읽는 값으로 바꿈
//   포트 1 = 보드(RAK3172, app.h APP_LORA_PORT) 1분 보고 · 포트 8 = Nucleo 벤치 0.2 이하(Wio-E5 기본 포트) · 포트 9 = Nucleo 벤치 0.3(11/25, SDP810 대기압 보정함)
//   포트 3 = 보드 횟수 보고(APP_CNT_PORT) · 포트 4 = 교정값 보고(APP_CAL_PORT, 설정 05 뒤 · 11/24)
// 서버 참고 구현 tools/report_decode.py 와 같은 눈금 · 같은 거부 규칙(버전과 길이가 짝이 맞아야 함)
// 11/24 v2: 24 · 26 번에 SDP810 차압 1분 평균(0.02 Pa) · DS18B20 온도(0.01 °C), 부호 있는 16비트, 0x8000 = 없음

var FLAGS = ["FLOWING", "VENTING", "DEPLETED", "LEAK_B", "OUT_OF_RANGE", "BATT_WARN", "BATT_QUALITY",
             "ZERO_UNSET", "TIME_UNSYNCED", "RESENT", "CLIPPED", "INCOMPLETE", "BOOT", "FAULT_RESET",
             "BATT_OFF", "BARO_HELD"];
var REPORT_PORTS = [1, 8, 9];
var COUNTS_PORT = 3;
var CAL_PORT = 4;
var CAL_NAMES = ["c_re_a", "c_re_b", "c_re_min", "throat_mm", "inlet_mm", "visc_upas", "visc_s_k", "molar_gmol"];

function u16(b, i) { return (b[i] << 8) | b[i + 1]; }
function s16(b, i) { var v = u16(b, i); return v >= 0x8000 ? v - 0x10000 : v; }
function u32(b, i) { return ((b[i] << 24) >>> 0) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]; }
function opt16(b, i, k) { var v = s16(b, i); return v === -32768 ? null : v / k; }
// float32 큰 쪽 먼저 (DataView 없이 — ChirpStack JS 엔진에서도 되게)
function f32(b, i) {
  var u = u32(b, i);
  var sign = (u >>> 31) ? -1 : 1, e = (u >>> 23) & 0xFF, m = u & 0x7FFFFF;
  if (e === 0) { return sign * m * Math.pow(2, -149); }
  if (e === 255) { return m ? NaN : sign * Infinity; }
  return sign * (1 + m / 8388608) * Math.pow(2, e - 127);
}

// 포트 8 만: 11/23 Nucleo 벤치 옛 형식(버전 1 + 꼬리 4 B = 28 B)도 받음 — 꼬리는 v2 24 · 26 번과 같은 자리 · 같은 눈금.
// 벤치를 v2 로 다시 굽기 전까지 ChirpStack 화면에서 값이 계속 보이게 하려는 것 (서버 풀이 tools/report_decode.py 는 거부)
function decodeReport(b, port) {
  var benchTail = port === 8 && b.length === 28 && b[0] === 1;
  var v2 = (b.length === 28 && b[0] === 2) || benchTail;
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
    ds18b20_c: v2 ? opt16(b, 26, 100) : null,   // DS18B20 가스관 온도 (v1 · 센서 없음 = null)
    // SDP810 값을 966 hPa 기준 대기압 보정했는지: 보드(포트 1) · Nucleo 벤치 0.3(포트 9) 함 · 옛 벤치(포트 8) 안 함 — 같은 칸이라도 뜻이 다름
    sdp810_pressure_corrected: port !== 8
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

// 교정값 보고 (포트 4, 34 B): 형식 1 · 넣은 칸 표시 · 지금 쓰는 값 8개 (Core/Inc/app.h APP_CFG_CAL)
function decodeCal(b) {
  if (b.length !== 34 || b[0] !== 1) {
    return { errors: ["교정값 보고 형식 버전 " + b[0] + " · 길이 " + b.length + " B 가 안 맞음 (34 B · 버전 1)"] };
  }
  var d = { set_fields: [] };
  for (var k = 0; k < 8; k++) {
    d[CAL_NAMES[k]] = Math.round(f32(b, 2 + 4 * k) * 1e6) / 1e6;
    if (b[1] & (1 << k)) { d.set_fields.push(CAL_NAMES[k]); }   // 넣은 칸 (나머지는 펌웨어 기본값)
  }
  return { data: d };
}

function decodeUplink(input) {
  var b = input.bytes;
  if (REPORT_PORTS.indexOf(input.fPort) >= 0) { return decodeReport(b, input.fPort); }
  if (input.fPort === COUNTS_PORT) { return decodeCounts(b); }
  if (input.fPort === CAL_PORT) { return decodeCal(b); }
  return { errors: ["모르는 포트 " + input.fPort + " (" + b.length + " B)"] };
}
