#!/usr/bin/env python3
from pathlib import Path
import sys
import os

sys.dont_write_bytecode = True
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/"etc"/"tools"))

import main
if __name__ == "__main__":
    sys.exit(main.main())

