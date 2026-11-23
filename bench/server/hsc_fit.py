"""HSC 를 SDP810 에 맞추는 보정값(기울기 · 오프셋) 계산.

두 센서가 T자 관으로 같은 압력을 받는 상태에서 쌓인 CSV 로그를 읽어,
SDP810 ≈ a × HSC + b 를 최소제곱으로 구하고 펌웨어에 넣을 값을 찍어 줌.

  python hsc_fit.py logs/2026-11-18/14.csv [더 많은 csv ...] [--from 14:10:00] [--to 14:20:00]
                    [--gain-ppm 1000000] [--offset-mpa 0]

--gain-ppm / --offset-mpa 에는 그 로그를 찍을 때 보드에 들어 있던 보정값을 넣음
(부팅 때 board.log 에 "HSC 보정: 기울기 ..., 오프셋 ..." 으로 찍힘. 안 찍혔으면 기본값 그대로).
새 보정값 = 그 위에 이번 결과를 겹친 값이라, 여러 번 반복해도 됨.

쓰는 줄: 두 센서 모두 '정상', |SDP810| ≤ 480 Pa, 앞뒤 1초 동안 SDP810 이 0.5 Pa 이상 안 흔들린 구간.
"""

import argparse
import csv
import statistics


def load(paths, t_from, t_to):
    rows = []
    for p in paths:
        with open(p, encoding="utf-8-sig") as f:
            for r in csv.DictReader(f):
                if r.get("hsc_state") != "정상" or r.get("sdp_state") != "정상":
                    continue
                hhmmss = r["time"][11:19]
                if (t_from and hhmmss < t_from) or (t_to and hhmmss > t_to):
                    continue
                rows.append((r["time"], float(r["hsc_pa"]), float(r["sdp_pa"])))
    rows.sort()
    return rows


def steady(rows, half=5, tol=0.5, tol_hsc=1.5):
    """앞뒤 half 줄(0.1초 주기면 ±0.5초) 동안 SDP810 폭이 tol Pa, HSC 폭이 tol_hsc Pa 이하인 줄만.
    (HSC 는 눈금이 0.15 Pa 라 조금 넓게 봄. 한쪽만 움직인 구간 = 관이 안 이어진 때라 빠짐)"""
    out = []
    for i in range(half, len(rows) - half):
        win = rows[i - half:i + half + 1]
        ys = [r[2] for r in win]
        xs = [r[1] for r in win]
        if max(ys) - min(ys) <= tol and max(xs) - min(xs) <= tol_hsc and abs(rows[i][2]) <= 480:
            out.append(rows[i])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--from", dest="t_from", help="HH:MM:SS 부터")
    ap.add_argument("--to", dest="t_to", help="HH:MM:SS 까지")
    ap.add_argument("--gain-ppm", type=int, default=1000000, help="로그 찍을 때 보드의 기울기")
    ap.add_argument("--offset-mpa", type=int, default=0, help="로그 찍을 때 보드의 오프셋")
    a = ap.parse_args()

    rows = steady(load(a.csv, a.t_from, a.t_to))
    if len(rows) < 50:
        raise SystemExit(f"쓸 수 있는 줄이 {len(rows)}개뿐임. 압력을 몇 초씩 멈춰 가며 더 기록할 것")
    x = [r[1] for r in rows]  # HSC (로그 값)
    y = [r[2] for r in rows]  # SDP810
    span = max(y) - min(y)
    mx, my = statistics.fmean(x), statistics.fmean(y)
    if span < 50:
        a1, b1 = 1.0, my - mx
        print(f"압력 폭이 {span:.1f} Pa 뿐이라 기울기는 못 구함. 오프셋만 맞춤")
    else:
        sxx = sum((xi - mx) ** 2 for xi in x)
        sxy = sum((xi - mx) * (yi - my) for xi, yi in zip(x, y))
        a1 = sxy / sxx
        b1 = my - a1 * mx
    res = [yi - (a1 * xi + b1) for xi, yi in zip(x, y)]
    print(f"쓴 줄 {len(rows)}개, SDP810 {min(y):.1f} ~ {max(y):.1f} Pa")
    print(f"SDP810 ≈ {a1:.6f} × HSC + {b1:+.3f} Pa,  남는 오차 표준편차 {statistics.pstdev(res):.3f} Pa, 최대 {max(map(abs, res)):.3f} Pa")

    # 압력 구간별로 얼마나 맞는지
    buckets = {}
    for xi, yi in zip(x, y):
        buckets.setdefault(round(yi / 50) * 50, []).append((xi, yi))
    print("\n  구간(Pa)   줄 수   SDP810 평균   HSC 평균(지금)   HSC 평균(보정 후)")
    for k in sorted(buckets):
        xs, ys = zip(*buckets[k])
        print(f"  {k:>7}   {len(xs):>5}   {statistics.fmean(ys):>11.2f}   {statistics.fmean(xs):>14.2f}   "
              f"{a1 * statistics.fmean(xs) + b1:>17.2f}")

    g0, o0 = a.gain_ppm / 1e6, a.offset_mpa / 1000.0
    g, o = a1 * g0, a1 * o0 + b1  # 지금 보정 위에 이번 결과를 겹침
    print("\n펌웨어 prj.conf 에 넣을 값:")
    print(f"CONFIG_BENCH_HSC_GAIN_PPM={round(g * 1e6)}")
    print(f"CONFIG_BENCH_HSC_OFFSET_MPA={round(o * 1000)}")


if __name__ == "__main__":
    main()
