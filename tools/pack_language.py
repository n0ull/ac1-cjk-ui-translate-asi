"""将 AnvilToolkit 导出物、译文词典和源字体打包为 AC1_Language.bin。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "modules"))
from language_pack import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
