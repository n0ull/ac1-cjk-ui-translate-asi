"""将 LG 主词典导出为可填写译文的 Excel 表格。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "modules"))
from translation_sheet import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
