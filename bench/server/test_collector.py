"""수집기 · 유량 계산 자동 시험.

  python test_collector.py            # 전부 (브로커가 없으면 통합 시험만 건너뜀)
  python test_collector.py -v         # 시험 이름까지 보기

무엇을 보나:
  1) 계산식 (venturi.py): 문서 예제 값, 따로 계산한 값과 대조, 제곱근 법칙, 밀도, 표준상태 환산, 0 이하 차압
  2) 줄 풀기 (parse_line): 정상 줄, SDP810 없는 줄, 오류 상태, 음수, 안내 줄
  3) 기록 (Writer): 1초 묶음 쓰기, 머리줄, 유량 열, 시간 바뀔 때 새 파일, JSON 꺼짐, latest.json, 옛 머리줄 파일 치우기
  4) 통합: 진짜 브로커에 가짜 보드 줄을 보내고 수집기가 CSV 로 남기는지 (중복 · 안내 줄 · 오류 줄 포함)
     실제 토픽과 안 섞이게 h563/selftest 토픽과 임시 폴더를 씀.
"""

import csv
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="bench_test_")
os.environ["BENCH_LOG_DIR"] = os.path.join(TMP, "unit")
os.environ.pop("BENCH_JSONL", None)
sys.path.insert(0, HERE)

import collector  # noqa: E402
import venturi  # noqa: E402

LINE_OK = ("[period 100 ms, read 51620 us] #198  HSC 정상 -0.21 Pa 28.3 C  |  "
           "ABP2 정상 20.4 Pa 24.97 C (st 0x40)  |  SDP810 정상 -0.05 Pa 25.7 C")


def by_hand(dp, pg, t_c, dt_mm, d_mm, c, m, p_atm):
    """문서 2 · 3절 식을 시험 안에서 따로 계산 (venturi.py 를 안 거침)."""
    T = t_c + 273.15
    p_abs = p_atm + pg
    rho = p_abs * m / (8.314 * T)
    area = math.pi * (dt_mm / 1000) ** 2 / 4
    beta = dt_mm / d_mm
    q = c * area * math.sqrt(2 * dp / rho) / math.sqrt(1 - beta ** 4) * 3600
    return q, q * (p_abs / 101325) * (273.15 / T), rho


class Venturi(unittest.TestCase):
    def setUp(self):
        self.cfg = venturi.load_config()

    def test_문서_예제_1_2(self):
        # 32.4 Pa, 정압 300 Pa, 22 °C, 고도 1,700 m, 목 4.62 mm, 입구 25 mm → 0.49 m³/h, 0.37 Nm³/h
        cfg = dict(venturi.DEFAULTS, throat_mm=4.62, inlet_mm=25.0, C=0.97, gas_molar_kg=0.02723,  # 문서 예제는 바이오가스
                   p_atm_pa=101325 * (1 - 2.25577e-5 * 1700) ** 5.25588)
        q, qn, rho = venturi.flow(cfg, 32.4, 300, 22)
        self.assertAlmostEqual(q, 0.49, places=2)
        self.assertAlmostEqual(qn, 0.37, places=2)
        self.assertAlmostEqual(rho, 0.919, places=3)

    def test_지금_설정은_실물_벤투리(self):
        # 실물: 입구 16 mm, 목 4 mm (β 0.25). 벤치 시험은 건조 공기
        self.assertEqual(self.cfg["inlet_mm"], 16.0)
        self.assertEqual(self.cfg["throat_mm"], 4.0)
        self.assertAlmostEqual(self.cfg["throat_mm"] / self.cfg["inlet_mm"], 0.25, places=6)
        self.assertAlmostEqual(self.cfg["gas_molar_kg"], 0.028965, places=6)

    def test_목_넓이만큼_유량이_바뀜(self):
        # 같은 차압이면 유량은 대략 목 넓이에 비례: 4 mm 는 3.5 mm 의 (4/3.5)² 배 (β⁴ 보정만큼 조금 다름)
        q4 = venturi.flow(self.cfg, 50, 0, 20)[0]
        q35 = venturi.flow(dict(self.cfg, throat_mm=3.5), 50, 0, 20)[0]
        self.assertAlmostEqual(q4 / q35, (4 / 3.5) ** 2, delta=0.01)

    def test_따로_계산한_값과_같음(self):
        c = self.cfg
        for dp in (0.6, 1, 10, 32.4, 100, 500, 1000):
            for pg, t in ((0, 20), (21, 25.5), (300, 35), (2000, 5)):
                want = by_hand(dp, pg, t, c["throat_mm"], c["inlet_mm"], c["C"], c["gas_molar_kg"], c["p_atm_pa"])
                got = venturi.flow(c, dp, pg, t)
                for w, g in zip(want, got):
                    self.assertAlmostEqual(g, w, delta=abs(w) * 1e-9 + 1e-12)

    def test_차압이_4배면_유량은_2배(self):
        q1 = venturi.flow(self.cfg, 25, 20, 25)[0]
        q4 = venturi.flow(self.cfg, 100, 20, 25)[0]
        self.assertAlmostEqual(q4 / q1, 2.0, places=9)

    def test_차압_0_이하는_유량_0(self):
        self.assertEqual(venturi.flow(self.cfg, 0, 20, 25)[0], 0.0)
        self.assertEqual(venturi.flow(self.cfg, 0.49, 20, 25)[0], 0.0)          # 컷오프 0.5 Pa 미만
        self.assertGreater(venturi.flow(self.cfg, 0.5, 20, 25)[0], 0.0)
        self.assertEqual(venturi.flow(self.cfg, -3.2, 20, 25)[0], 0.0)

    def test_값이_없으면_None(self):
        self.assertEqual(venturi.flow(self.cfg, None, 20, 25), (None, None, None))
        self.assertEqual(venturi.flow(self.cfg, 10, 20, None), (None, None, None))

    def test_공기_밀도는_0도_1기압에서_1_292(self):
        cfg = dict(self.cfg, p_atm_pa=101325)  # 지금 설정 = 공기
        rho = venturi.flow(cfg, 10, 0, 0)[2]
        self.assertAlmostEqual(rho, 101325 * 0.028965 / (8.314 * 273.15), places=9)
        self.assertAlmostEqual(rho, 1.292, places=3)

    def test_기본값도_공기(self):
        self.assertAlmostEqual(venturi.DEFAULTS["gas_molar_kg"], 0.028965, places=6)

    def test_바이오가스로_바꾸면_밀도_1_215(self):
        rho = venturi.flow(dict(self.cfg, p_atm_pa=101325, gas_molar_kg=0.02723), 10, 0, 0)[2]
        self.assertAlmostEqual(rho, 1.215, places=3)

    # ---- C(Re): 2026-11-20 계량기 비교 시험으로 맞춘 센서별 유출계수 ----
    def test_C_Re_는_센서별로_시험값과_맞음(self):
        # 시험 점(HSC 표시 0.6 m³/h): 계량기 48 L 대비 HSC 50.21 L → 실제 유량 0.6 × 48 / 50.21 = 0.5736 m³/h.
        # 시험 때 표시값은 예전 기준(대기압 101,325 Pa 고정 · SDP810 칩 온도 26.3 °C · C 0.97)으로 계산한 것.
        old = dict(self.cfg, p_atm_pa=101325)
        dp = 0.0
        for _ in range(400):  # 예전 기준으로 HSC 가 0.6 m³/h 를 보이는 차압 찾기 (약 111 Pa)
            dp += 1.0
            if venturi.flow(old, dp, 0, 26.3)[0] >= 0.6:
                break
        # 같은 차압을 새 기준(BME280 대기압 102,040 Pa · 가스 온도 25.0 °C)으로 계산하면 계량기 유량이 나와야 함
        q, _, _, c, re = venturi.flow(self.cfg, dp, 0, 25.0, sensor="hsc", detail=True, p_atm_pa=102040)
        self.assertAlmostEqual(re, 3460, delta=60)
        self.assertAlmostEqual(c, 0.931, delta=0.002)
        self.assertAlmostEqual(q, 0.6 * 48 / 50.21, delta=0.004)

    def test_C_Re_는_유량이_클수록_커짐(self):
        cs = [venturi.flow(self.cfg, dp, 0, 25, sensor="hsc", detail=True)[3] for dp in (30, 60, 120, 240)]
        self.assertEqual(cs, sorted(cs))
        self.assertLess(cs[0], 0.97)

    def test_시험_범위_밖은_끝_값으로_고정(self):
        m = {"a": 0.5362, "b": 0.1103, "re_min": 1699, "re_max": 5665}      # log 식: 양 끝 고정
        cfg = dict(self.cfg, C_re={"hsc": m})
        lo = m["a"] + m["b"] * math.log10(m["re_min"])
        hi = m["a"] + m["b"] * math.log10(m["re_max"])
        self.assertAlmostEqual(venturi.flow(cfg, 2.0, 0, 25, sensor="hsc", detail=True)[3], lo, places=9)    # 아주 작은 유량
        self.assertAlmostEqual(venturi.flow(cfg, 1000.0, 0, 25, sensor="hsc", detail=True)[3], hi, places=9)  # 큰 유량
        inv = {"model": "inv", "a": 0.9774, "b": -158.9, "re_min": 1725}     # inv 식: 아래만 고정, 위는 a 로 수렴
        cfg = dict(self.cfg, C_re={"hsc": inv})
        self.assertAlmostEqual(venturi.flow(cfg, 2.0, 0, 25, sensor="hsc", detail=True)[3], 0.9774 - 158.9 / 1725, places=9)
        c_big, re_big = venturi.flow(cfg, 1000.0, 0, 25, sensor="hsc", detail=True)[3:]
        self.assertAlmostEqual(c_big, 0.9774 - 158.9 / re_big, places=9)
        self.assertLess(c_big, 0.9774)

    def test_C_Re_가_없거나_센서를_안_주면_고정_C(self):
        cfg = {k: v for k, v in self.cfg.items() if k != "C_re"}
        self.assertEqual(venturi.flow(cfg, 50, 0, 25, sensor="hsc", detail=True)[3], cfg["C"])
        self.assertEqual(venturi.flow(self.cfg, 50, 0, 25, detail=True)[3], self.cfg["C"])
        q_fixed = venturi.flow(cfg, 50, 0, 25)[0]
        q, _, _, c, _ = venturi.flow(self.cfg, 50, 0, 25, sensor="hsc", detail=True)
        self.assertAlmostEqual(q, q_fixed * c / self.cfg["C"], places=12)

    def test_공기_점성(self):
        self.assertAlmostEqual(venturi.air_viscosity(25.0), 1.837e-5, delta=0.01e-5)  # 표 값 1.849e-5 와 1 % 안
        self.assertAlmostEqual(venturi.air_viscosity(0.0), 1.716e-5, places=9)

    def test_0도_1기압이면_표준상태_유량이_그대로(self):
        cfg = dict(self.cfg, p_atm_pa=101325)
        q, qn, _ = venturi.flow(cfg, 50, 0, 0)
        self.assertAlmostEqual(q, qn, places=12)

    def test_따뜻하고_기압_낮으면_표준상태가_더_작음(self):
        cfg = dict(self.cfg, p_atm_pa=82000)
        q, qn, _ = venturi.flow(cfg, 50, 300, 22)
        self.assertLess(qn, q)
        self.assertAlmostEqual(qn / q, (82300 / 101325) * (273.15 / 295.15), places=9)

    def test_정압이_오르면_밀도가_커져_유량이_줄어듦(self):
        self.assertLess(venturi.flow(self.cfg, 50, 5000, 25)[0], venturi.flow(self.cfg, 50, 0, 25)[0])

    def test_측정_줄에서_두_센서_유량(self):
        r = collector.parse_line(LINE_OK.replace("-0.21 Pa", "32.40 Pa").replace("-0.05 Pa", "8.10 Pa"), 0)
        f = venturi.flows(self.cfg, r)
        want_h = venturi.flow(self.cfg, 32.4, 20.4, 25.7, sensor="hsc")   # 정압은 ABP2, 온도는 SDP810, C 는 센서별
        want_s = venturi.flow(self.cfg, 8.1, 20.4, 25.7, sensor="sdp")
        self.assertAlmostEqual(f["hsc"]["m3h"], want_h[0], places=4)
        self.assertAlmostEqual(f["sdp"]["m3h"], want_s[0], places=4)
        self.assertAlmostEqual(f["hsc"]["m3h"] / f["sdp"]["m3h"], want_h[0] / want_s[0], places=3)
        self.assertAlmostEqual(f["rho"], want_h[2], places=4)

    def test_SDP810_이_죽으면_SDP_유량만_비고_HSC_는_계속(self):
        r = collector.parse_line(LINE_OK.replace("SDP810 정상 -0.05 Pa 25.7 C", "SDP810 응답 없음")
                                 .replace("-0.21 Pa", "50.00 Pa"), 0)
        f = venturi.flows(self.cfg, r)
        self.assertIsNone(f["sdp"]["m3h"])
        self.assertGreater(f["hsc"]["m3h"], 0)  # 온도는 다른 센서(HSC 28.3 °C) 것으로 대신


LINE_ENV = LINE_OK + "  |  BME280 정상 102039.6 Pa 23.92 C 21.5 %  |  DS18B20 정상 24.31 C"


class Calibration(unittest.TestCase):
    """교정값 파일(venturi_cal.json): 쓰기 · 덮어쓰기 · 자동으로 다시 읽기 · 이상한 값 거절."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=TMP)
        self.path = os.path.join(self.dir, "venturi.json")
        with open(self.path, "w", encoding="utf-8") as f:
            json.dump({"throat_mm": 4.0, "inlet_mm": 16.0, "C": 0.97,
                       "C_re": {"_설명": "x", "hsc": {"a": 0.5362, "b": 0.1103, "re_min": 1699, "re_max": 5665}}}, f)
        self._env = os.environ.pop("BENCH_VENTURI_CAL", None)

    def tearDown(self):
        if self._env is not None:
            os.environ["BENCH_VENTURI_CAL"] = self._env

    def test_교정값이_없으면_venturi_json_그대로(self):
        cfg = venturi.load_config(self.path)
        self.assertEqual(cfg["C_re"]["hsc"]["a"], 0.5362)
        self.assertIsNone(venturi.cal_summary(cfg)["at"])

    def test_쓰면_그_센서만_덮어씀(self):
        venturi.save_cal("sdp", {"model": "inv", "a": 0.93671, "b": -134.55, "re_min": 1774}, "시험", path=self.path)
        cfg = venturi.load_config(self.path)
        self.assertEqual(cfg["C_re"]["sdp"]["model"], "inv")
        self.assertEqual(cfg["C_re"]["hsc"]["a"], 0.5362)            # 안 쓴 센서는 venturi.json 값
        self.assertNotIn("_설명", cfg["C_re"])
        self.assertIsNotNone(venturi.cal_summary(cfg)["at"])
        c = venturi.flow(cfg, 100, 0, 25, sensor="sdp", detail=True)
        self.assertAlmostEqual(c[3], 0.93671 - 134.55 / c[4], places=9)

    def test_이전_값은_history_에_남음(self):
        venturi.save_cal("hsc", {"model": "inv", "a": 0.97, "b": -150, "re_min": 1700}, path=self.path)
        cal = venturi.save_cal("hsc", {"model": "inv", "a": 0.98, "b": -160, "re_min": 1700}, path=self.path)
        self.assertEqual(cal["C_re"]["hsc"]["a"], 0.98)
        self.assertEqual((cal["history"][0]["sensor"], cal["history"][0]["a"]), ("hsc", 0.97))

    def test_이상한_값은_거절(self):
        for bad in ({"model": "inv", "a": 0.979, "b": -900, "re_min": 1700},      # 낮은 Re 에서 C 0.45
                    {"model": "inv", "a": 1.3, "b": -100, "re_min": 1700},        # C 가 1 을 넘음
                    {"model": "log", "a": 0.5, "b": 0.1, "re_min": 1700},         # log 인데 위 끝이 없음
                    {"model": "poly", "a": 0.9, "b": 0, "re_min": 1700},
                    {"model": "inv", "a": "x", "b": -100, "re_min": 1700},
                    {"model": "inv", "a": 0.97, "b": -100, "re_min": 0}):
            with self.assertRaises(ValueError, msg=str(bad)):
                venturi.save_cal("hsc", bad, path=self.path)
        with self.assertRaises(ValueError):
            venturi.save_cal("abp2", {"model": "inv", "a": 0.97, "b": -100, "re_min": 1700}, path=self.path)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "venturi_cal.json")))

    def test_망가진_교정_파일은_무시(self):
        with open(os.path.join(self.dir, "venturi_cal.json"), "w", encoding="utf-8") as f:
            f.write("{ 깨짐")
        self.assertEqual(venturi.load_config(self.path)["C_re"]["hsc"]["a"], 0.5362)
        with open(os.path.join(self.dir, "venturi_cal.json"), "w", encoding="utf-8") as f:
            json.dump({"C_re": {"hsc": {"model": "inv", "a": 5, "b": 0, "re_min": 1}}}, f)   # 손으로 넣은 이상한 값
        self.assertEqual(venturi.load_config(self.path)["C_re"]["hsc"]["a"], 0.5362)

    def test_파일이_바뀌면_refresh_가_다시_읽음(self):
        cfg = venturi.load_config(self.path)
        self.assertIs(venturi.refresh(cfg), cfg)                      # 1초 안에는 파일도 안 봄
        cfg["_seen"] = 0.0
        self.assertIs(venturi.refresh(cfg), cfg)                      # 봤지만 안 바뀜
        venturi.save_cal("hsc", {"model": "inv", "a": 0.97739, "b": -158.91, "re_min": 1725}, path=self.path)
        self.assertIs(venturi.refresh(cfg), cfg)                      # 방금 봤으니 1초 뒤에
        cfg["_seen"] = 0.0
        new = venturi.refresh(cfg)
        self.assertIsNot(new, cfg)
        self.assertEqual(new["C_re"]["hsc"]["a"], 0.97739)

    def test_flows_에_Re_가_나옴(self):
        cfg = venturi.load_config(self.path)
        r = collector.parse_line(LINE_OK.replace("-0.21 Pa", "111.00 Pa").replace("-0.05 Pa", "111.00 Pa"), 0)
        f = venturi.flows(cfg, r)
        self.assertAlmostEqual(f["hsc"]["Re"], 3400, delta=120)
        self.assertEqual(f["sdp"]["C"], 0.97)                          # 식이 없는 센서는 고정 C, Re 는 없음
        self.assertIsNone(f["sdp"]["Re"])


class NewSensors(unittest.TestCase):
    """BME280(대기압) · DS18B20(가스 온도)이 붙은 줄: 풀기와 계산 반영."""

    def setUp(self):
        self.cfg = venturi.load_config()

    def test_새_칸_풀기(self):
        r = collector.parse_line(LINE_ENV, 0)
        self.assertEqual(r["bme"], {"ok": True, "state": "정상", "pa": 102039.6, "c": 23.92, "rh": 21.5})
        self.assertEqual(r["ds"], {"ok": True, "state": "정상", "pa": None, "c": 24.31, "rh": None})
        self.assertEqual(r["sdp"], {"ok": True, "state": "정상", "pa": -0.05, "c": 25.7})  # 뒤 칸이 SDP810 에 안 섞임
        self.assertEqual(r["abp2"]["status"], "0x40")

    def test_옛_줄에는_새_칸이_없음(self):
        r = collector.parse_line(LINE_OK, 0)
        self.assertIsNone(r["bme"])
        self.assertIsNone(r["ds"])

    def test_응답_없음과_모르는_칸(self):
        r = collector.parse_line(LINE_OK + "  |  BME280 응답 없음  |  DS18B20 이전 값 85.00 C  |  XYZ 1 2 3", 0)
        self.assertFalse(r["bme"]["ok"])
        self.assertIsNone(r["bme"]["pa"])
        self.assertEqual((r["ds"]["ok"], r["ds"]["state"], r["ds"]["c"]), (False, "이전 값", 85.0))
        self.assertEqual(r["sdp"]["pa"], -0.05)

    def test_대기압과_가스_온도가_계산에_들어감(self):
        r = collector.parse_line(LINE_ENV.replace("-0.21 Pa", "32.40 Pa"), 0)
        f = venturi.flows(self.cfg, r)
        self.assertEqual(f["p_atm"], {"pa": 102039.6, "src": "bme"})
        self.assertEqual(f["t"], {"c": 24.31, "src": "ds"})
        want = venturi.flow(self.cfg, 32.4, 20.4, 24.31, sensor="hsc", p_atm_pa=102039.6)
        self.assertAlmostEqual(f["hsc"]["m3h"], want[0], places=4)
        self.assertAlmostEqual(f["rho"], (102039.6 + 20.4) * 0.028965 / (8.314 * (24.31 + 273.15)), places=4)
        # 대기압이 높아지면(101,325 → 102,040) 밀도가 커져 실제 유량은 줄고 표준 유량은 늘어남
        old = venturi.flow(self.cfg, 32.4, 20.4, 24.31, sensor="hsc")
        self.assertLess(want[0], old[0])
        self.assertGreater(want[1], old[1])

    def test_센서가_죽으면_대체값(self):
        r = collector.parse_line(LINE_OK.replace("-0.21 Pa", "32.40 Pa") + "  |  BME280 응답 없음  |  DS18B20 응답 없음", 0)
        f = venturi.flows(self.cfg, r)
        self.assertEqual(f["p_atm"], {"pa": self.cfg["p_atm_pa"], "src": "fixed"})
        self.assertEqual(f["t"], {"c": 25.7, "src": "sdp"})          # DS18B20 다음은 SDP810
        r["sdp"] = None
        self.assertEqual(venturi.flows(self.cfg, r)["t"], {"c": 28.3, "src": "hsc"})

    def test_말이_안_되는_대기압은_안_씀(self):
        r = collector.parse_line(LINE_ENV.replace("102039.6 Pa", "0.0 Pa"), 0)
        self.assertEqual(venturi.flows(self.cfg, r)["p_atm"]["src"], "fixed")
        fixed = dict(self.cfg, p_atm_source="fixed")
        self.assertEqual(venturi.flows(fixed, collector.parse_line(LINE_ENV, 0))["p_atm"]["src"], "fixed")

    def test_CSV_에_새_열(self):
        for k in ("bme_state", "bme_pa", "bme_c", "bme_rh", "ds_state", "ds_c"):
            self.assertIn(k, collector.CSV_HEAD)
        self.assertEqual(len(collector.CSV_HEAD), 30)


class Parse(unittest.TestCase):
    def test_정상_줄(self):
        r = collector.parse_line(LINE_OK, 1234)
        self.assertEqual((r["seq"], r["period_ms"], r["read_us"], r["t"]), (198, 100, 51620, 1234))
        self.assertEqual(r["hsc"], {"ok": True, "state": "정상", "pa": -0.21, "c": 28.3})
        self.assertEqual(r["abp2"], {"ok": True, "state": "정상", "pa": 20.4, "c": 24.97, "status": "0x40"})
        self.assertEqual(r["sdp"], {"ok": True, "state": "정상", "pa": -0.05, "c": 25.7})

    def test_SDP810_없는_옛_줄(self):
        r = collector.parse_line("[period 1000 ms, read 13201 us] #12  HSC 정상 0.05 Pa 31.6 C  |  "
                                 "ABP2 정상 21.1 Pa 28.27 C (st 0x40)", 0)
        self.assertIsNone(r["sdp"])
        self.assertEqual(r["abp2"]["pa"], 21.1)

    def test_오류_상태는_값이_빔(self):
        r = collector.parse_line("[period 100 ms, read 900 us] #7  HSC 응답 없음  |  ABP2 센서 오류 비트  |  "
                                 "SDP810 응답 없음", 0)
        for k in ("hsc", "abp2", "sdp"):
            self.assertFalse(r[k]["ok"])
            self.assertIsNone(r[k]["pa"])
        self.assertEqual(r["hsc"]["state"], "응답 없음")
        self.assertEqual(r["abp2"]["state"], "센서 오류 비트")

    def test_범위_밖은_값은_있지만_정상은_아님(self):
        r = collector.parse_line("[period 100 ms, read 900 us] #8  HSC 정상 1.00 Pa 28.0 C  |  "
                                 "ABP2 범위 밖 6000.0 Pa 25.00 C (st 0x40)  |  SDP810 범위 밖 546.13 Pa 25.0 C", 0)
        self.assertFalse(r["abp2"]["ok"])
        self.assertEqual(r["abp2"]["pa"], 6000.0)
        self.assertFalse(r["sdp"]["ok"])
        self.assertEqual(r["sdp"]["pa"], 546.13)

    def test_안내_줄은_측정이_아님(self):
        for t in ("HSC 영점 6.81 Pa (32번 평균). 이제 찍는 HSC 차압은 이만큼 뺀 값.",
                  "[스캔] I2C1 HSC : 0x25 0x28  (2개)", "[네트] MQTT 연결됨 (172.30.1.1:1883, 토픽 h563/bench/line)",
                  "SDP810 (I2C1 0x25) 연속 측정 시작", "", "garbage ###"):
            self.assertIsNone(collector.parse_line(t, 0), t)


def row(seq, t_ms, hsc=1.0, sdp=1.0, abp2=20.0):
    return collector.parse_line(
        f"[period 100 ms, read 51620 us] #{seq}  HSC 정상 {hsc:.2f} Pa 28.0 C  |  "
        f"ABP2 정상 {abp2:.1f} Pa 25.00 C (st 0x40)  |  SDP810 정상 {sdp:.2f} Pa 25.0 C", t_ms)


def read_csv(path):
    with open(path, encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


class Write(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=TMP)
        collector.LOG_DIR = self.dir
        self.w = collector.Writer()
        self.t0 = int(time.mktime((2026, 11, 19, 10, 0, 5, 0, 0, -1)) * 1000)

    def tearDown(self):
        self.w.lockf.close()

    def test_묶어서_한_번에_쓰고_머리줄은_한_번(self):
        for i in range(10):
            self.w.sample(row(i + 1, self.t0 + i * 100, hsc=10 + i, sdp=10 + i))
        path = os.path.join(self.dir, "2026-11-19", "10.csv")
        self.assertFalse(os.path.exists(path))          # flush 전에는 파일이 안 생김
        self.w.flush()
        rows = read_csv(path)
        self.assertEqual(len(rows), 10)
        self.assertEqual(list(rows[0].keys()), collector.CSV_HEAD)
        for i in range(10, 20):
            self.w.sample(row(i + 1, self.t0 + i * 100))
        self.w.flush()
        rows = read_csv(path)
        self.assertEqual([int(r["seq"]) for r in rows], list(range(1, 21)))
        with open(path, encoding="utf-8-sig") as f:
            self.assertEqual(sum(1 for ln in f if ln.startswith("time,")), 1)

    def test_유량_열이_계산식과_같음(self):
        self.w.sample(row(1, self.t0, hsc=100.0, sdp=25.0, abp2=300.0))
        self.w.flush()
        r = read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))[0]
        cfg = collector.VENTURI
        qh = venturi.flow(cfg, 100.0, 300.0, 25.0, sensor="hsc")
        qs = venturi.flow(cfg, 25.0, 300.0, 25.0, sensor="sdp")
        self.assertAlmostEqual(float(r["flow_hsc_m3h"]), qh[0], places=4)
        self.assertAlmostEqual(float(r["flow_hsc_nm3h"]), qh[1], places=4)
        self.assertAlmostEqual(float(r["flow_sdp_m3h"]), qs[0], places=4)
        self.assertAlmostEqual(float(r["flow_sdp_nm3h"]), qs[1], places=4)
        self.assertAlmostEqual(float(r["rho_kgm3"]), qh[2], places=4)
        self.assertAlmostEqual(float(r["flow_hsc_m3h"]) / float(r["flow_sdp_m3h"]), qh[0] / qs[0], places=3)

    def test_시간이_바뀌면_새_파일(self):
        t59 = int(time.mktime((2026, 11, 19, 10, 59, 59, 0, 0, -1)) * 1000)
        for i in range(20):                              # 10:59:59.0 ~ 11:00:00.9
            self.w.sample(row(i + 1, t59 + i * 100))
        self.w.flush()
        a = read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))
        b = read_csv(os.path.join(self.dir, "2026-11-19", "11.csv"))
        self.assertEqual((len(a), len(b)), (10, 10))
        self.assertEqual(a[-1]["time"], "2026-11-19T10:59:59.900")
        self.assertEqual(b[0]["time"], "2026-11-19T11:00:00.000")

    def test_JSON_기록은_기본으로_꺼져_있고_latest_는_남음(self):
        self.w.sample(row(1, self.t0))
        self.w.sample(row(2, self.t0 + 100, sdp=7.5))
        self.w.flush()
        self.assertFalse(os.path.exists(os.path.join(self.dir, "2026-11-19", "10.jsonl")))
        with open(os.path.join(self.dir, "latest.json"), encoding="utf-8") as f:
            last = json.load(f)
        self.assertEqual(last["seq"], 2)
        self.assertEqual(last["sdp"]["pa"], 7.5)
        self.assertIn("flow", last)

    def test_오류_줄은_빈_칸으로_남고_유량도_빔(self):
        r = collector.parse_line("[period 100 ms, read 900 us] #7  HSC 응답 없음  |  ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  "
                                 "SDP810 응답 없음", self.t0)
        self.w.sample(r)
        self.w.flush()
        c = read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))[0]
        self.assertEqual((c["hsc_state"], c["hsc_pa"], c["flow_hsc_m3h"]), ("응답 없음", "", ""))
        self.assertEqual((c["sdp_state"], c["sdp_pa"], c["flow_sdp_m3h"]), ("응답 없음", "", ""))
        self.assertEqual(c["abp2_pa"], "20.0")

    def test_열_구성이_다른_옛_파일은_옆으로_치움(self):
        day = os.path.join(self.dir, "2026-11-19")
        os.makedirs(day)
        with open(os.path.join(day, "10.csv"), "w", encoding="utf-8-sig") as f:
            f.write("time,seq,old_column\n2026-11-19T10:00:00.000,1,x\n")
        self.w.sample(row(5, self.t0))
        self.w.flush()
        self.assertEqual(len(read_csv(os.path.join(day, "10.csv"))), 1)
        self.assertEqual(len([n for n in os.listdir(day) if n.startswith("10_old")]), 1)

    def test_같은_폴더는_한_수집기만_씀(self):
        self.w.sample(row(1, self.t0))
        self.w.flush()                                   # 첫 수집기가 잠금을 잡음
        w2 = collector.Writer()
        try:
            self.assertFalse(w2.own())
            w2.sample(row(2, self.t0 + 100))
            w2.flush()
            self.assertEqual(len(read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))), 1)
        finally:
            w2.lockf.close()


class LockedFile(unittest.TestCase):
    """다른 프로그램이 latest.json 을 열어 둔 순간에도 수집기가 안 죽는지 (윈도우에서 실제로 죽었던 경우)."""

    def test_latest_json_open_elsewhere(self):
        w = collector.Writer()
        w.sample(collector.parse_line(LINE_OK, int(time.time() * 1000)))
        w.flush()
        latest = os.path.join(collector.LOG_DIR, "latest.json")
        with open(latest, encoding="utf-8") as held:  # 읽는 쪽이 잡고 있는 동안
            w.sample(collector.parse_line(LINE_OK.replace("#198", "#199"), int(time.time() * 1000)))
            w.flush()  # 예전에는 여기서 PermissionError 로 죽었음
            held.read()
        w.sample(collector.parse_line(LINE_OK.replace("#198", "#200"), int(time.time() * 1000)))
        w.flush()
        with open(latest, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["seq"], 200)  # 풀린 뒤에는 다시 최신 값


class Total(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=TMP)
        collector.LOG_DIR = self.dir
        self.w = collector.Writer()
        self.t0 = int(time.mktime((2026, 11, 19, 10, 0, 0, 0, 0, -1)) * 1000)

    def tearDown(self):
        self.w.lockf.close()

    def test_1초_동안_일정_유량이면_유량_곱하기_1초(self):
        for i in range(10):
            self.w.sample(row(i + 1, self.t0 + i * 100, hsc=100.0, sdp=25.0, abp2=0.0))
        self.w.flush()
        q_h = venturi.flow(collector.VENTURI, 100.0, 0.0, 25.0, sensor="hsc")
        q_s = venturi.flow(collector.VENTURI, 25.0, 0.0, 25.0, sensor="sdp")
        last = read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))[-1]
        self.assertAlmostEqual(float(last["total_hsc_m3"]), q_h[0] / 3600, places=6)
        self.assertAlmostEqual(float(last["total_hsc_nm3"]), q_h[1] / 3600, places=6)
        self.assertAlmostEqual(float(last["total_sdp_m3"]), q_s[0] / 3600, places=6)
        rows = read_csv(os.path.join(self.dir, "2026-11-19", "10.csv"))
        self.assertTrue(all(float(a["total_hsc_m3"]) <= float(b["total_hsc_m3"]) for a, b in zip(rows, rows[1:])))

    def test_흐름이_없거나_센서_오류면_안_늘어남(self):
        self.w.sample(row(1, self.t0, hsc=0.0, sdp=-0.03))
        self.w.sample(collector.parse_line("[period 100 ms, read 900 us] #2  HSC 응답 없음  |  "
                                           "ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 응답 없음", self.t0 + 100))
        self.w.flush()
        for r in read_csv(os.path.join(self.dir, "2026-11-19", "10.csv")):
            self.assertEqual(float(r["total_hsc_m3"]), 0.0)
            self.assertEqual(float(r["total_sdp_m3"]), 0.0)

    def test_껐다_켜도_이어지고_날짜가_바뀌면_오늘만_0(self):
        for i in range(10):
            self.w.sample(row(i + 1, self.t0 + i * 100, hsc=400.0, sdp=400.0))
        self.w.flush()
        self.w.lockf.close()
        with open(os.path.join(self.dir, "totals.json"), encoding="utf-8") as f:
            first = json.load(f)["total"]["hsc"]["m3"]
        self.assertGreater(first, 0)
        self.w = collector.Writer()                      # 수집기 다시 켬
        tomorrow = int(time.mktime((2026, 11, 20, 0, 0, 1, 0, 0, -1)) * 1000)
        for i in range(10):
            self.w.sample(row(100 + i, tomorrow + i * 100, hsc=400.0, sdp=400.0))
        self.w.flush()
        with open(os.path.join(self.dir, "latest.json"), encoding="utf-8") as f:
            t = json.load(f)["totals"]
        self.assertAlmostEqual(t["hsc"]["m3"], 2 * first, places=6)        # 누적은 이어짐
        self.assertAlmostEqual(t["today"]["hsc"]["m3"], first, places=6)   # 오늘은 새로
        self.assertEqual(t["day"], "2026-11-20")


class FakeWriter:
    def __init__(self):
        self.rows, self.notes, self.fast = [], [], []

    def sample(self, r):
        self.rows.append(r)

    def note(self, text):
        self.notes.append(text)

    def hsc_fast(self, seq, step_ms, vals, now_ms):
        self.fast.append((seq, vals))


def meas(seq):
    return (f"[period 100 ms, read 51620 us] #{seq}  HSC 정상 1.00 Pa 28.0 C  |  "
            f"ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 정상 1.00 Pa 25.0 C")


class SerialInput(unittest.TestCase):
    """랜선 대신 USB 시리얼로 받는 길 (Intake · serial_loop)."""

    def setUp(self):
        self.w = FakeWriter()
        self.i = collector.Intake(self.w)

    def test_시리얼_줄은_src_가_serial_이고_색_코드와_프롬프트는_뗌(self):
        self.assertEqual(self.i.feed("\x1b[1;32muart:~$ \x1b[m" + meas(7) + "\r", "serial", 1000), "line")
        r = self.w.rows[0]
        self.assertEqual((r["seq"], r["src"], r["period_ms"], r["hsc"]["pa"]), (7, "serial", 100, 1.0))
        self.assertTrue(r["raw"].startswith("[period"))
        self.assertEqual(self.i.feed(meas(8), "mqtt", 1100), "line")
        self.assertEqual(self.w.rows[1]["src"], "mqtt")

    def test_두_길로_같은_줄이_오면_하나만_남음(self):
        # 랜선 10, 11 이 먼저 오고 시리얼 10, 11 이 늦게 옴 (순서가 엇갈려도 하나씩만)
        got = [self.i.feed(meas(n), src, t) for n, src, t in
               ((10, "mqtt", 1000), (11, "mqtt", 1100), (10, "serial", 1150), (11, "serial", 1160), (12, "serial", 1200),
                (12, "mqtt", 1210))]
        self.assertEqual(got, ["line", "line", "dup", "dup", "line", "dup"])
        self.assertEqual([(r["seq"], r["src"]) for r in self.w.rows], [(10, "mqtt"), (11, "mqtt"), (12, "serial")])

    def test_보드가_다시_켜져_번호가_처음부터여도_받음(self):
        self.i.feed(meas(5), "serial", 1000)
        self.assertEqual(self.i.feed(meas(5), "serial", 1000 + 3001), "line")  # 3초 넘게 지난 같은 번호는 새 줄
        self.assertEqual(len(self.w.rows), 2)

    def test_랜선으로_온_시각은_랜선_줄만_바꿈(self):
        self.i.feed(meas(1), "serial", 5000)
        self.assertEqual((self.i.last_line, self.i.last_lan), (5.0, 0.0))
        self.i.feed(meas(2), "mqtt", 6000)
        self.assertEqual((self.i.last_line, self.i.last_lan), (6.0, 6.0))
        self.i.feed(meas(2), "serial", 6050)                                   # 중복이어도 "줄이 오는 중" 으로는 침
        self.assertEqual((self.i.last_line, self.i.last_lan), (6.05, 6.0))

    def test_네트_안내는_10분에_한_번만_다른_안내는_다_남음(self):
        net = "[네트] {}초째 주소 없음: 랜선 링크 안 붙음 (선·포트 확인), MAC 02:80:E1:96:D3:3E"
        for k in range(1, 61):                                                 # 10초마다 10분 동안
            self.i.feed(net.format(k * 10), "serial", k * 10_000)
        self.assertEqual(len(self.w.notes), 1)
        # 측정 줄과 겹쳐 찍혀 앞머리가 잘린 줄도 같은 안내로 봄
        self.i.feed("\ufffd선 링크 안 붙음 (선·포트 확인), MAC 02:80:E1:96:D3:3E", "serial", 605_000)
        self.i.feed("포트 확인), MAC 02:80:E1:96:D3:3E", "serial", 606_000)
        self.assertEqual(len(self.w.notes), 1)
        # 랜선을 꽂아 링크 상태가 바뀌면 10분이 안 됐어도 남김
        self.i.feed("[네트] 620초째 주소 없음: 랜선 링크 붙음, MAC 02:80:E1:96:D3:3E. 그 포트가 DHCP 를 안 주거나 응답이 늦음",
                    "serial", 607_000)
        self.assertEqual(len(self.w.notes), 2)
        self.i.feed("[네트] MQTT 끊김 (-128). 줄은 쌓아 두고 다시 붙음", "serial", 608_000)   # 다른 [네트] 안내는 매번
        self.i.feed("[네트] MQTT 끊김 (-128). 줄은 쌓아 두고 다시 붙음", "serial", 609_000)
        self.assertEqual(len(self.w.notes), 4)
        self.w.notes.clear()
        self.i.feed(net.format(600), "serial", 609_900)                        # 처음 남긴 지 아직 10분이 안 됨
        self.assertEqual(len(self.w.notes), 0)
        self.i.feed(net.format(610), "serial", 611_000)                        # 10분이 지나면 다시 한 번
        self.assertEqual(len(self.w.notes), 1)
        for t in (700_000, 701_000):                                           # 영점은 연달아 잡아도 다 남아야 함
            self.i.feed("HSC 영점 5.44 Pa (32번 평균). 이제 찍는 HSC 차압은 이만큼 뺀 값.", "serial", t)
        self.assertEqual(len(self.w.notes), 3)
        self.assertEqual(self.w.rows, [])

    def test_빈_줄과_HSC_빠른_줄(self):
        self.assertIsNone(self.i.feed("uart:~$ \r", "serial", 1))
        self.assertEqual(self.i.feed("[hsc 100 ms] #3 1.0 x 2.5", "serial", 2), "fast")
        self.assertEqual(self.i.feed("[hsc 100 ms] #3 1.0 x 2.5", "mqtt", 3), "dup")
        self.assertEqual(self.w.fast, [(3, [1.0, None, 2.5])])

    def test_시리얼_읽는_스레드(self):
        """가짜 포트로: 줄이 안 올 때만 엶, 첫 줄은 버림, 받은 줄은 브로커로 다시 보냄, 쉼 파일이 있으면 안 엶."""
        import threading
        import types

        opened, relayed = [], []
        chunks = [("잘린 첫 줄 #999  HSC 정상 9 Pa 9 C  |  ABP2 정상 9 Pa 9 C\r\n" + meas(21) + "\r\n").encode(),
                  ("안내 줄\r\n" + meas(22) + "\r\n" + meas(23)[:40]).encode(), (meas(23)[40:] + "\r\n").encode()]

        class SerialException(Exception):
            pass

        class FakePort:
            def __init__(self, port, baud, timeout=None):
                opened.append(port)
                self.left = list(chunks) if len(opened) == 1 else []
                self.in_waiting = 0

            def read(self, n):
                if self.left:
                    return self.left.pop(0)
                raise SerialException("USB 빠짐")

            def close(self):
                pass

        old = (sys.modules.get("serial"), collector.SERIAL_PORT, collector.SERIAL_AFTER_S, collector.SERIAL_PAUSE)
        sys.modules["serial"] = types.SimpleNamespace(Serial=FakePort, SerialException=SerialException)
        pause = os.path.join(TMP, "serial.pause")
        collector.SERIAL_PORT, collector.SERIAL_AFTER_S, collector.SERIAL_PAUSE = "FAKE", 0.3, pause
        try:
            open(pause, "w").close()
            self.i.last_line = 0                                               # 줄이 한참 안 온 상태
            threading.Thread(target=collector.serial_loop, args=(self.i, relayed.append), daemon=True).start()
            time.sleep(1.5)
            self.assertEqual(opened, [])                                       # 쉼 파일이 있는 동안은 안 엶
            os.remove(pause)
            end = time.time() + 5
            while len(self.w.rows) < 3 and time.time() < end:
                time.sleep(0.05)
            self.assertEqual([(r["seq"], r["src"]) for r in self.w.rows], [(21, "serial"), (22, "serial"), (23, "serial")])
            self.assertEqual(relayed, [meas(21), meas(22), meas(23)])          # 안내 줄은 다시 안 보냄
            self.assertIn("안내 줄", self.w.notes)
            self.assertEqual(opened, ["FAKE"])                                 # 방금 줄이 왔으니 바로 다시 열지 않음
            self.assertTrue(any("놓음" in n and "USB 빠짐" in n for n in self.w.notes))
        finally:
            collector.SERIAL_AFTER_S = 1e9                                     # 남은 스레드는 더 안 엶
            if old[0] is None:
                sys.modules.pop("serial", None)
            else:
                sys.modules["serial"] = old[0]
            collector.SERIAL_PORT, collector.SERIAL_PAUSE = old[1], old[3]


class Integration(unittest.TestCase):
    """진짜 브로커 → 수집기 프로세스 → CSV. 브로커나 계정이 없으면 건너뜀."""

    TOPIC = "h563/selftest"

    def setUp(self):
        self.cred_path = os.environ.get("BENCH_MQTT_CRED") or next(
            (p for p in (os.path.join(HERE, "credentials.txt"), os.path.join(HERE, "..", "secrets", "credentials.txt"))
             if os.path.exists(p)), None)
        if not self.cred_path:
            self.skipTest("계정 파일 없음")
        import paho.mqtt.client as paho
        with open(self.cred_path, encoding="utf-8") as f:
            cred = dict(ln.strip().split("=", 1) for ln in f if "=" in ln)
        self.pub = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id=f"selftest-pub-{os.getpid()}")
        self.pub.username_pw_set(cred.get("user"), cred.get("password"))
        try:
            self.pub.connect(os.environ.get("BENCH_MQTT_HOST", "127.0.0.1"), int(os.environ.get("BENCH_MQTT_PORT", "1883")))
        except OSError:
            self.skipTest("브로커에 못 붙음")
        self.pub.loop_start()
        self.dir = tempfile.mkdtemp(dir=TMP)
        env = dict(os.environ, BENCH_MQTT_TOPIC=self.TOPIC, BENCH_LOG_DIR=self.dir, BENCH_MQTT_CRED=self.cred_path,
                   PYTHONIOENCODING="utf-8", BENCH_SERIAL_PORT="")  # 시험 중엔 진짜 포트를 안 엶
        self.proc = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "collector.py")], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(2.5)

    def tearDown(self):
        if getattr(self, "proc", None):
            self.proc.kill()
            self.proc.wait()
            self.proc.stdout.close()
        if getattr(self, "pub", None):
            self.pub.loop_stop()
            self.pub.disconnect()

    def send(self, text):
        self.pub.publish(self.TOPIC + "/line", text).wait_for_publish()

    def test_보드_줄이_CSV_로_남음(self):
        self.pub.publish(self.TOPIC + "/status", "online 10.0.0.9").wait_for_publish()
        self.send("HSC 영점 6.81 Pa (32번 평균). 이제 찍는 HSC 차압은 이만큼 뺀 값.")
        t_first = time.time()
        for i in range(1, 31):                           # 0.1초마다 30줄, 차압이 계단처럼 오름
            dp = 0.0 if i <= 10 else 100.0 if i <= 20 else 400.0
            self.send(f"[period 100 ms, read 51620 us] #{i}  HSC 정상 {dp:.2f} Pa 28.0 C  |  "
                      f"ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 정상 {dp:.2f} Pa 25.0 C")
            if i == 15:                                  # 같은 줄이 한 번 더 옴 → 하나만 남아야 함
                self.send(f"[period 100 ms, read 51620 us] #15  HSC 정상 100.00 Pa 28.0 C  |  "
                          f"ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 정상 100.00 Pa 25.0 C")
            time.sleep(0.1)
        self.send("[period 100 ms, read 900 us] #31  HSC 응답 없음  |  ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 응답 없음")
        self.send("이건 측정 줄이 아님")
        # USB 시리얼에서 읽어 다시 보낸 줄(<토픽>/usb)도 남고, 같은 번호가 랜선으로 또 오면 하나만
        usb = ("[period 100 ms, read 51620 us] #32  HSC 정상 0.00 Pa 28.0 C  |  "
               "ABP2 정상 20.0 Pa 25.00 C (st 0x40)  |  SDP810 정상 0.00 Pa 25.0 C")
        self.pub.publish(self.TOPIC + "/usb", usb).wait_for_publish()
        self.send(usb)
        time.sleep(2.5)

        files = [os.path.join(r, n) for r, _, ns in os.walk(self.dir) for n in ns if n.endswith(".csv")]
        self.assertTrue(files, "CSV 가 안 생김")
        rows = [r for f in sorted(files) for r in read_csv(f)]
        self.assertEqual([int(r["seq"]) for r in rows], list(range(1, 33)))        # 32줄, 중복 없음, 순서대로
        self.assertEqual([rows[30]["src"], rows[31]["src"]], ["mqtt", "serial"])          # 32번은 usb 토픽으로 먼저 옴
        q0, q100, q400 = (float(rows[i]["flow_sdp_m3h"]) for i in (4, 14, 24))
        self.assertEqual(q0, 0.0)
        self.assertAlmostEqual(q100, venturi.flow(collector.VENTURI, 100, 20, 25, sensor="sdp")[0], places=4)
        self.assertAlmostEqual(q400, venturi.flow(collector.VENTURI, 400, 20, 25, sensor="sdp")[0], places=4)
        self.assertEqual(rows[30]["hsc_state"], "응답 없음")
        self.assertEqual(rows[30]["flow_hsc_m3h"], "")
        # 1초마다 묶어 쓰므로 3초 남짓 동안 파일은 몇 번만 열림: 받은 시각이 0.1초 간격으로 찍혔는지만 확인
        ts = [time.mktime(time.strptime(r["time"][:19], "%Y-%m-%dT%H:%M:%S")) + int(r["time"][20:]) / 1000 for r in rows[:30]]
        self.assertLess(max(ts) - min(ts), 4.0)
        self.assertGreater(min(ts), t_first - 1)
        with open(os.path.join(self.dir, "board.log"), encoding="utf-8") as f:
            log = f.read()
        self.assertIn("[상태] online 10.0.0.9", log)
        self.assertIn("HSC 영점 6.81 Pa", log)
        self.assertIn("이건 측정 줄이 아님", log)
        self.assertFalse([n for r, _, ns in os.walk(self.dir) for n in ns if n.endswith(".jsonl")])
        # 적산: 100 Pa 10줄 + 400 Pa 10줄 = 각 유량 × 1초
        want = sum(venturi.flow(collector.VENTURI, dp, 20, 25, sensor="sdp")[0] for dp in (100.0, 400.0)) / 3600
        self.assertAlmostEqual(float(rows[29]["total_sdp_m3"]), want, places=5)
        self.assertEqual(rows[30]["total_hsc_m3"], rows[29]["total_hsc_m3"])          # 오류 줄은 안 더함


if __name__ == "__main__":
    try:
        unittest.main(verbosity=1)
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
