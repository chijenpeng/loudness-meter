"""Cross-check independent C++ spectral and Python time-domain/reference anchors."""
import json
import math
import subprocess
import sys

from reference_math import anchors

cpp = json.loads(subprocess.check_output([sys.argv[1], '--anchors-json'], text=True))
python = anchors()
assert cpp.keys() == python.keys()
for key, expected in python.items():
    actual = cpp[key]
    if not math.isfinite(actual) or abs(actual - expected) > 1e-8:
        raise AssertionError(f'{key}: C++ {actual}, Python {expected}')
    print(f'{key}: independent references agree, delta={actual - expected:.3g}')
print('Reference-only cross-check passed; production code was not linked.')
