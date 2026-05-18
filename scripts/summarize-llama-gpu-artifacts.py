#!/usr/bin/env python3
"""Compatibility wrapper for the relocated llama GPU artifact summarizer."""
from __future__ import annotations

from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGET_REL = "scripts/maintenance/summarize-llama-gpu-artifacts.py"
TARGET = ROOT / TARGET_REL
sys.argv[0] = str(TARGET)
runpy.run_path(str(TARGET), run_name="__main__")
