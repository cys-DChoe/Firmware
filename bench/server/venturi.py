"""벤투리 유량 계산 (문서 "바이오가스 계량기 계산식" 2 · 3절의 식).

  Q   = C × Aₜ × √(2 × ΔP / ρ) ÷ √(1 − β⁴)          [m³/s]  Aₜ = π dₜ² / 4, β = dₜ / D
  ρ   = P_abs × M ÷ (8.314 × T)                       [kg/m³] M = 공기 28.965 g/mol (벤치 시험 기준)
                                                               현장 바이오가스(메탄 60 %, CO₂ 40 %)는 M = 27.23 g/mol
  P_abs = P_atm + P_g                                  P_g = ABP2 정압(게이지)
                                                       P_atm = BME280 대기압 (없으면 venturi.json 의 p_atm_pa)
                                                       T = 관 속 온도 센서 DS18B20 (없으면 SDP810 → HSC → ABP2 칩 온도)
  Qₙ  = Q × (P_abs ÷ 101,325) × (273.15 ÷ T)          [Nm³/s] 0 °C · 1기압 환산

유출계수 C (센서별, venturi.json 의 C_re 가 있을 때):
  C 를 고정값 대신 목 레이놀즈 수 Re 의 식으로 씀:  C = a + b × log₁₀(Re)
  Re = 4 ρ Q₀ ÷ (π dₜ μ),  Q₀ = 고정 C(기본 0.97)로 계산한 그 센서의 유량,  μ = 공기 점성(Sutherland 식)
  a · b 는 2026-11-20 벤치 시험(가스계량기 대비 0.3~1.0 m³/h 7점)에서 센서마다 맞춘 값. 그 시험에서
  유량을 고정 C 로 계산한 HSC 표시값으로 맞췄으므로, Re 도 같은 방식(Q₀ 기준)으로 잡아 반복 계산이 필요 없음.
  시험한 범위 밖(re_min 미만 · re_max 초과)은 늘려 쓰지 않고 끝 값으로 고정함.
  C_re 가 없거나 sensor 를 안 주면 예전처럼 고정 C.
  식 모양은 둘: "log" (기본) C = a + b × log₁₀(Re),  "inv" C = a + b ÷ Re (b 는 음수, a 는 Re 가 아주 클 때의 C).
  "inv" 는 Re 가 커지면 a 로 수렴해서 re_max 없이 써도 됨. 시험 점에 더 잘 맞아(잔차 1.2 → 0.9 %) 2026-11-20 밤부터 씀.

교정값 (venturi_cal.json, venturi.json 옆):
  실시간 페이지의 "교정" 에서 계량기 비교 시험 점들로 맞춘 C_re 를 save_cal() 이 여기에 씀. 있으면 venturi.json 의
  C_re 를 센서별로 덮어씀. 수집기와 MCP 서버는 refresh() 로 1초마다 파일이 바뀌었는지 보고 다시 읽음 (껐다 켤 필요 없음).

치수 · 가스 · 대기압은 venturi.json 에서 읽음 (없으면 아래 DEFAULTS). 문서 예제 값이 기본이라
실제 벤투리 치수가 정해지면 venturi.json 만 고치면 됨.
차압이 0 이하이거나 dp_cutoff_pa(기본 0.5 Pa) 미만이면 유량 0 (저유량 컷오프).
"""

import json
import math
import os
import time

DEFAULTS = {
    "throat_mm": 4.62,        # 목 지름 dₜ (문서 예제 값)
    "inlet_mm": 25.0,         # 입구 지름 D (문서 예제 값)
    "C": 0.97,                # 유출계수 (벤치 시험 전 예제 값)
    "gas_molar_kg": 0.028965,  # 공기 평균 분자량 kg/mol. 현장 바이오가스(메탄 60 %, CO₂ 40 %)면 0.02723
    "p_atm_pa": 101325.0,     # 대기압 대체값 (BME280 이 없거나 고장일 때). 고도 h m 이면 101325 × (1 − 2.25577e-5 × h)^5.25588
    "p_atm_source": "bme",    # 대기압: bme = BME280 측정값(없으면 p_atm_pa) | fixed = 항상 p_atm_pa
    "temp_source": "ds",      # 가스 온도로 쓸 센서: ds(DS18B20, 관 속) | sdp | hsc | abp2. 없으면 이 순서로 다음 것
    "dp_cutoff_pa": 0.5,      # 저유량 컷오프: 이 차압 미만은 유량 0 (0 점 잡음이 적산에 쌓이지 않게)
}


SENSORS = ("hsc", "sdp")


def _paths(path=None):
    path = path or os.environ.get("BENCH_VENTURI", os.path.join(os.path.dirname(os.path.abspath(__file__)), "venturi.json"))
    cal = os.environ.get("BENCH_VENTURI_CAL", os.path.join(os.path.dirname(os.path.abspath(path)), "venturi_cal.json"))
    return path, cal


def _stamp(paths):
    out = []
    for p in paths:
        try:
            st = os.stat(p)
            out.append((st.st_mtime_ns, st.st_size))
        except OSError:
            out.append(None)
    return tuple(out)


def load_config(path=None):
    """venturi.json 을 읽고, 옆에 venturi_cal.json(페이지에서 적용한 교정값)이 있으면 C_re 를 센서별로 덮어씀."""
    cfg = dict(DEFAULTS)
    path, cal_path = _paths(path)
    try:
        with open(path, encoding="utf-8") as f:
            cfg.update({k: v for k, v in json.load(f).items() if not k.startswith("_")})
    except (OSError, ValueError):
        pass
    try:
        with open(cal_path, encoding="utf-8") as f:
            cal = json.load(f)
        models = {k: v for k, v in (cal.get("C_re") or {}).items() if k in SENSORS and c_model_ok(v) is None}
        if models:
            cfg["C_re"] = dict({k: v for k, v in (cfg.get("C_re") or {}).items() if not k.startswith("_")}, **models)
            cfg["cal_at"] = cal.get("applied_at")
    except (OSError, ValueError, AttributeError):
        pass
    cfg["_path"], cfg["_stamp"], cfg["_seen"] = path, _stamp((path, cal_path)), time.monotonic()
    return cfg


def refresh(cfg):
    """설정 파일(venturi.json · venturi_cal.json)이 바뀌었으면 다시 읽은 설정을, 아니면 받은 것을 그대로 돌려줌.
    파일 확인은 1초에 한 번만. 수집기 · MCP 서버가 줄을 계산하기 전에 부름."""
    now = time.monotonic()
    if now - cfg.get("_seen", 0.0) < 1.0:
        return cfg
    cfg["_seen"] = now
    path = cfg.get("_path")
    if path is None or _stamp(_paths(path)) == cfg.get("_stamp"):
        return cfg
    return load_config(path)


def c_model(model, re):
    """C(Re) 식 값. re_min · re_max 가 있으면 그 밖은 끝 값으로 고정."""
    lo, hi = model.get("re_min"), model.get("re_max")
    if lo:
        re = max(re, lo)
    if hi:
        re = min(re, hi)
    if model.get("model", "log") == "inv":
        return model["a"] + model["b"] / re
    return model["a"] + model["b"] * math.log10(re)


def c_model_ok(model):
    """교정값이 쓸 만한지. 괜찮으면 None, 아니면 이유(글)."""
    try:
        kind = model.get("model", "log")
        a, b, lo, hi = float(model["a"]), float(model["b"]), model.get("re_min"), model.get("re_max")
    except (AttributeError, KeyError, TypeError, ValueError):
        return "a · b 가 숫자가 아님"
    if kind not in ("log", "inv"):
        return "식 모양은 log 나 inv"
    if not all(math.isfinite(x) for x in (a, b)):
        return "a · b 가 숫자가 아님"
    if not lo or lo <= 0 or (hi is not None and hi <= lo):
        return "re_min 은 0 보다 크고 re_max 보다 작아야 함"
    if kind == "log" and not hi:
        return "log 식은 re_max 가 있어야 함 (끝없이 커짐)"
    for re in (lo, hi or 1e7):
        c = c_model(dict(model, a=a, b=b), re)
        if not 0.5 <= c <= 1.05:
            return f"Re {re:.0f} 에서 C 가 {c:.3f} (0.5 ~ 1.05 밖)"
    return None


def save_cal(sensor, model, note="", path=None):
    """한 센서의 교정값을 venturi_cal.json 에 씀 (이전 값은 history 에 20개까지 남김). 돌려줌: 쓴 뒤의 파일 내용."""
    if sensor not in SENSORS:
        raise ValueError("sensor 는 hsc 나 sdp")
    why = c_model_ok(model)
    if why:
        raise ValueError(why)
    _, cal_path = _paths(path)
    try:
        with open(cal_path, encoding="utf-8") as f:
            cal = json.load(f)
    except (OSError, ValueError):
        cal = {}
    cal.setdefault("_설명", "실시간 페이지 '교정' 에서 적용한 유출계수 식. venturi.json 의 C_re 를 센서별로 덮어씀. 이 파일을 지우면 venturi.json 값으로 돌아감.")
    now = time.strftime("%Y-%m-%dT%H:%M:%S")
    old = (cal.get("C_re") or {}).get(sensor)
    if old:
        cal["history"] = ([dict(old, sensor=sensor, until=now)] + list(cal.get("history") or []))[:20]
    entry = {"model": model.get("model", "log"), "a": round(float(model["a"]), 6), "b": round(float(model["b"]), 6),
             "re_min": round(float(model["re_min"]), 1)}
    if model.get("re_max"):
        entry["re_max"] = round(float(model["re_max"]), 1)
    entry["at"] = now
    if note:
        entry["note"] = str(note)[:200]
    cal.setdefault("C_re", {})[sensor] = entry
    cal["applied_at"] = now
    tmp = cal_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline=chr(10)) as f:
        json.dump(cal, f, ensure_ascii=False, indent=2)
    for _ in range(20):  # 윈도우: 다른 프로그램이 읽는 순간에는 바꿔치기가 거절됨
        try:
            os.replace(tmp, cal_path)
            break
        except PermissionError:
            time.sleep(0.05)
    else:
        raise OSError("venturi_cal.json 을 못 바꿈 (다른 프로그램이 잡고 있음)")
    return cal


def cal_summary(cfg):
    """지금 쓰는 유출계수 설정 (페이지에 보여 줄 것): 고정 C, 센서별 식, 적용 시각."""
    keep = ("model", "a", "b", "re_min", "re_max", "at")
    return {"C": cfg["C"],
            "C_re": {k: {x: y for x, y in v.items() if x in keep} for k, v in (cfg.get("C_re") or {}).items() if k in SENSORS},
            "at": cfg.get("cal_at")}


def air_viscosity(t_c):
    """공기 점성 Pa·s (Sutherland 식). 25 °C 에서 약 1.84e-5."""
    T = t_c + 273.15
    return 1.716e-5 * (T / 273.15) ** 1.5 * (273.15 + 110.4) / (T + 110.4)


def discharge_coef(cfg, sensor, q0_m3s, rho, t_c):
    """그 센서에 쓸 C. C_re 에 sensor 가 있으면 C(Re), 없으면 고정 C. 돌려줌: (C, Re 또는 None)."""
    model = (cfg.get("C_re") or {}).get(sensor) if sensor else None
    if not model or q0_m3s <= 0:
        return cfg["C"], None
    mu = cfg.get("viscosity_pa_s") or air_viscosity(t_c)
    re = 4.0 * rho * q0_m3s / (math.pi * (cfg["throat_mm"] / 1000.0) * mu)
    return c_model(model, re), re  # 시험 범위 밖은 끝 값으로 (c_model)


def flow(cfg, dp_pa, pg_pa, t_c, sensor=None, detail=False, p_atm_pa=None):
    """한 순간의 유량. dp_pa 차압, pg_pa 정압(게이지, 없으면 0), t_c 가스 온도(°C), sensor "hsc" | "sdp" (C 고를 때),
    p_atm_pa 측정한 대기압(없으면 설정의 p_atm_pa).
    돌려줌: (m³/h, Nm³/h, 밀도 kg/m³). 값이 없으면 (None, None, None). detail=True 면 끝에 (C, Re) 도 붙임."""
    if dp_pa is None or t_c is None:
        return (None, None, None, None, None) if detail else (None, None, None)
    T = t_c + 273.15
    p_abs = (cfg["p_atm_pa"] if p_atm_pa is None else p_atm_pa) + (pg_pa or 0.0)
    rho = p_abs * cfg["gas_molar_kg"] / (8.314 * T)
    dt = cfg["throat_mm"] / 1000.0
    beta = cfg["throat_mm"] / cfg["inlet_mm"]
    area = math.pi * dt * dt / 4.0
    if dp_pa < cfg.get("dp_cutoff_pa", 0.0):  # 0 점 근처 잡음은 흐름이 아님 (계산식 문서: 0.45 Pa 는 측정 불가)
        dp_pa = 0.0
    q0 = cfg["C"] * area * math.sqrt(2.0 * max(dp_pa, 0.0) / rho) / math.sqrt(1.0 - beta ** 4)  # m³/s, 고정 C
    c, re = discharge_coef(cfg, sensor, q0, rho, t_c)
    q = q0 * c / cfg["C"]
    qh = q * 3600.0
    qnh = qh * (p_abs / 101325.0) * (273.15 / T)
    return (qh, qnh, rho, c, re) if detail else (qh, qnh, rho)


P_ATM_MIN, P_ATM_MAX = 30000.0, 110000.0  # BME280 측정 범위(300~1100 hPa). 이 밖이면 고장으로 보고 대체값
T_ORDER = ("ds", "sdp", "hsc", "abp2")    # 가스 온도 후보 (고른 것이 없을 때 이 순서)


def atm_pressure(cfg, r):
    """계산에 쓸 대기압과 출처: (Pa, "bme" | "fixed"). BME280 이 정상이고 범위 안이면 그 값, 아니면 p_atm_pa."""
    b = r.get("bme") or {}
    if cfg.get("p_atm_source", "bme") == "bme" and b.get("ok") and b.get("pa") is not None             and P_ATM_MIN <= b["pa"] <= P_ATM_MAX:
        return b["pa"], "bme"
    return cfg["p_atm_pa"], "fixed"


def gas_temp(cfg, r):
    """계산에 쓸 가스 온도와 출처: (°C, "ds" | "sdp" | "hsc" | "abp2") 또는 (None, None).
    temp_source 로 고른 센서가 정상이면 그 값, 아니면 T_ORDER 순서로 처음 정상인 것."""
    first = cfg.get("temp_source", "ds")
    for k in (first,) + tuple(x for x in T_ORDER if x != first):
        x = r.get(k) or {}
        if x.get("ok") and x.get("c") is not None:
            return x["c"], k
    return None, None


def flows(cfg, r):
    """측정 줄 dict(hsc/abp2/sdp/bme/ds) 하나로 두 차압 센서 각각의 유량을 구함.
    p_atm · t 에는 계산에 실제로 쓴 대기압 · 가스 온도와 그 출처가 들어감."""
    a, h, s = r.get("abp2") or {}, r.get("hsc") or {}, r.get("sdp") or {}
    t_c, t_src = gas_temp(cfg, r)
    p_atm, p_src = atm_pressure(cfg, r)
    pg = a.get("pa") if a.get("ok") else None
    out = {"p_atm": {"pa": round(p_atm, 1), "src": p_src}, "t": {"c": t_c, "src": t_src}}
    for key, sen in (("sdp", s), ("hsc", h)):
        qh, qnh, rho, c, re = flow(cfg, sen.get("pa") if sen.get("ok") else None, pg, t_c, sensor=key, detail=True,
                                   p_atm_pa=p_atm)
        out[key] = {"m3h": None if qh is None else round(qh, 5), "nm3h": None if qnh is None else round(qnh, 5),
                    "C": None if c is None else round(c, 4),
                    "Re": None if re is None else round(re)}  # Re 는 고정 C 유량 기준 (교정 점을 잡을 때 씀)
        if rho is not None:
            out["rho"] = round(rho, 5)
    out.setdefault("rho", None)
    return out


if __name__ == "__main__":
    # 문서 예제 1 · 2 확인: 32.4 Pa, 정압 300 Pa, 22 °C, 고도 1,700 m → 0.49 m³/h, 0.37 Nm³/h
    c = dict(DEFAULTS, p_atm_pa=101325 * (1 - 2.25577e-5 * 1700) ** 5.25588)
    qh, qnh, rho = flow(c, 32.4, 300, 22)
    print(f"예제: {qh:.3f} m³/h, {qnh:.3f} Nm³/h, 밀도 {rho:.3f} kg/m³")
