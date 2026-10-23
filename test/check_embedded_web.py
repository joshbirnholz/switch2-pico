"""Check the final firmware image, catching linker/optimizer removal of the UI."""
from pathlib import Path
import sys

page = Path(__file__).resolve().parents[1] / "web" / "index.html"
image = Path(sys.argv[1])
expected = page.read_bytes()
if expected not in image.read_bytes():
    raise SystemExit(f"{image}: embedded configuration page is missing or outdated")
print(f"{image}: embedded web page verified ({len(expected)} bytes)")
