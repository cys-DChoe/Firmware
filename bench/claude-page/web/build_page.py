"""breadboard.tpl.html → ../breadboard.html (아티팩트로 올리는 파일).

- 글꼴: 샘튼 브랜드 글꼴 Pretendard GOV(가변, OFL) 를 페이지에 쓰인 글자만 남겨 woff2 data URI 로 넣음.
  아티팩트는 Google Fonts 말고 다른 글꼴 주소를 못 쓰기 때문. 글자 목록은 이 템플릿 + MCP 서버 코드(서버가 보내는 안내 문구) + ASCII.
- 로고: 브랜드 가이드 PDF 에서 뽑은 샘튼 마크 · 글자 벡터(samton_logo_paths.txt).

실행:  .venv\\Scripts\\python.exe web\\build_page.py   (fonttools, brotli 필요)
"""

import base64
import io
import os

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
TPL = os.path.join(HERE, "breadboard.tpl.html")
OUT = os.path.join(HERE, "..", "breadboard.html")
FONT = os.path.join(HERE, "fonts", "PretendardGOVVariable.woff2")
LOGO = os.path.join(HERE, "samton_logo_paths.txt")
EXTRA_TEXT = [os.path.join(HERE, "..", "breadboard_server.py")]


def logo_paths():
    lines = open(LOGO, encoding="utf-8").read().splitlines()
    out = {}
    for i, ln in enumerate(lines):
        if ln.startswith("mark ") or ln.startswith("word "):
            out[ln.split()[0]] = lines[i + 1].strip()
    return out["mark"], out["word"]


def font_b64(text):
    chars = set(text)
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.update("·—–−±°³ₜβ⁴√ΔρΣσ…")
    opts = subset.Options()
    opts.flavor = "woff2"
    opts.layout_features = ["kern", "liga", "calt", "tnum", "ccmp", "locl", "mark", "mkmk"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    font = TTFont(FONT)
    sub = subset.Subsetter(opts)
    sub.populate(text="".join(sorted(chars)))
    sub.subset(font)
    buf = io.BytesIO()
    font.flavor = "woff2"
    font.save(buf)
    return base64.b64encode(buf.getvalue()).decode("ascii"), len(buf.getvalue()), len(chars)


FONT_URL = "https://unpkg.com/pretendard-gov@1.3.9/dist/web/variable/woff2/PretendardGOVVariable.woff2"


def main():
    if not os.path.exists(FONT):
        raise SystemExit(f"글꼴 파일이 없음: {FONT}\n받을 곳: {FONT_URL} (5.4 MB, OFL) → web/fonts/ 에 두고 다시 실행")
    tpl = open(TPL, encoding="utf-8").read()
    text = tpl
    for p in EXTRA_TEXT:
        try:
            text += open(p, encoding="utf-8").read()
        except OSError:
            pass
    mark, word = logo_paths()
    b64, size, n = font_b64(text)
    html = tpl.replace("__FONT__", b64).replace("__MARK__", mark).replace("__WORD__", word)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print(f"글자 {n}개, 글꼴 {size / 1024:.0f} KB, 페이지 {len(html.encode('utf-8')) / 1024:.0f} KB → {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
