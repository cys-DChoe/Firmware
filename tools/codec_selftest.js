// ChirpStack 코덱(bench/nucleo_lora/chirpstack_codec.js)을 맥 시험 벡터와 맞대 봄 — tests/host/run.sh 가 node 가 있으면 돌림
// node tools/codec_selftest.js <코덱 js> <cal_vectors.txt>   (11/24 포트 4 교정값 보고 · 11/25 포트별 SDP810 보정 표시)
var fs = require("fs");
eval(fs.readFileSync(process.argv[2], "utf8"));
var bad = 0, n = 0;
fs.readFileSync(process.argv[3], "utf8").split("\n").forEach(function (line) {
  if (line.indexOf("K:") !== 0) { return; }
  var parts = line.slice(2).split("|");
  var hex = parts[0], bytes = [];
  for (var i = 0; i < hex.length; i += 2) { bytes.push(parseInt(hex.substr(i, 2), 16)); }
  var r = decodeUplink({ fPort: 4, bytes: bytes });
  n++;
  if (r.errors) { bad++; console.log("  FAIL 코덱 오류 " + r.errors); return; }
  var has = parseInt(parts[1], 10), set = 0;
  CAL_NAMES.forEach(function (name, k) { if (r.data.set_fields.indexOf(name) >= 0) { set |= 1 << k; } });
  if (set !== has) { bad++; console.log("  FAIL 코덱 넣은 칸 " + set + " / C " + has); }
  CAL_NAMES.forEach(function (name, k) {
    n++;
    var want = parseFloat(parts[2 + k]), got = r.data[name];
    if (Math.abs(got - want) > 1e-6 * Math.max(1, Math.abs(want)) + 1e-6) {
      bad++;
      console.log("  FAIL 코덱 " + name + ": " + got + " / C " + want);
    }
  });
});
// 1분 보고 포트별 SDP810 보정 표시 (11/25: 보드 1 · 벤치 0.3 = 9 는 보정함, 옛 벤치 8 은 안 함)
var rep = [2, 0, 1, 106, 250, 184, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 200, 125, 170, 0, 0, 128, 0, 128, 0];
[[1, true], [8, false], [9, true]].forEach(function (pc) {
  n++;
  var r = decodeUplink({ fPort: pc[0], bytes: rep });
  if (r.errors || r.data.sdp810_pressure_corrected !== pc[1]) {
    bad++;
    console.log("  FAIL 코덱 포트 " + pc[0] + " 보정 표시 " + (r.errors ? r.errors : r.data.sdp810_pressure_corrected));
  }
});
console.log("  코덱 풀기 대조: " + (n - bad) + " / " + n + " 통과");
process.exit(bad ? 1 : 0);
