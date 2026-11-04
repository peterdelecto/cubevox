"""Tests for decal_core. Importing this package puts the add-in folder on the path."""

import os
import sys

_ADDIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Decal")
if _ADDIN not in sys.path:
    sys.path.insert(0, _ADDIN)
