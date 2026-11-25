// ChirpStack 코덱(bench/nucleo_lora/chirpstack_codec.js)을 맥 시험 벡터와 맞대 봄 — tests/host/run.sh 가 node 가 있으면 돌림
// node tools/codec_selftest.js <코덱 js> <cal_vectors.txt>   (11/24, 지금은 포트 4 교정값 보고)
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
console.log("  코덱 풀기 대조: " + (n - bad) + " / " + n + " 통과");
process.exit(bad ? 1 : 0);
