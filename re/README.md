# Reverse-engineering kit

Everything here is for static analysis of `bin\win32\nwmain.exe`, build 26c6e573
(PE timestamp 0x68e408bc). If the game updates, re-check `DESIGN.md` §12a against
the new exe before trusting any offset.

`ghidra_proj/`, `decompiles/` and `data/` are local only and kept out of the public
repo, because they're derived from the game's binary. Build your own project from your
copy of the game with the scripts here.

## Contents
- `ghidra_proj/` holds the analysed Ghidra 12.2 project (name `nwn`, program `nwmain.exe`). Analysis took a long time, so keep it.
- `ghidra_scripts/` holds the headless scripts:
  - `Decomp.java`: arguments are an output file, then addresses or symbol names. It writes the decompiled C.
  - `DecompRange.java`: arguments are an output file, a start and an end address. It decompiles every function in that range, which is handy for one class's methods.
  - `Callers.java`, `StrXref.java`, `FuncAround.java`, `DumpSyms.java`, `TracyXref.java`: cross-reference and symbol helpers.
- `decompiles/` holds the saved Ghidra output:
  - `design/`: camera, drive and objects.
  - `quickbar/`: the quickbar panel and buttons, item and spell names.
  - `dialog/`: the conversation window and its replies.
  - `ui/`: GUI, input, NUI, Nuklear, server messages, `WinMessageHandler`, SDL main.
  - `movement/`: client and server drive, camera follow behaviour, interaction flags.
- `tools/` holds Python helpers (they need `pefile` and `capstone`):
  - `nwdis.py <exe> <va...>`: disassembles short functions.
  - `fdis.py <exe> <va> <bytes> [filter]`: disassembles a range, optionally filtered.
  - `exports.py <exe> <substr...>`: searches the full export table. pefile's own parser stops at 16k exports, so use this instead.
  - `callers.py <exe> <va...>`: finds call and jmp sites by scanning for rel32 targets.
  - `xref.py <exe> <va...>`: finds rip-relative lea and mov references, for example to a string.
  - `vt.py`: dumps vtables.
  - `thread_sampler.cpp`: samples the busiest thread of a hung process for RIP plus a stack scan. Build it with a small CMake project. It found the freeze bug.
- `data/`:
  - `strings.txt`: every string in the exe. The "rva" column is really a file offset; add 0xC00 to get the `.rdata` address.
  - `keymap.2da`

## Setup
- **Ghidra:** it lives at `C:\Ghidra\ghidra_12.2_PUBLIC`, with JAVA_HOME `C:\Program Files\Java\jdk-21.0.12.1`. Example decompile run:
  ```powershell
  $env:JAVA_HOME='C:\Program Files\Java\jdk-21.0.12.1'
  & 'C:\Ghidra\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat' '<abs path>\re\ghidra_proj' nwn -process nwmain.exe -noanalysis -readOnly -scriptPath '<abs path>\re\ghidra_scripts' -postScript Decomp.java '<out.c>' 1406ed720 SomeSymbol
  ```
  - Each run takes about 1–2 minutes, so batch functions together.
  - Copy the project before running two analyses at once.
  - Ghidra headless rejects paths that contain a dot-folder such as `.claude`, and `analyzeHeadless.bat` breaks on paths with spaces (this install's path has them). Run it through a junction, e.g. `New-Item -ItemType Junction C:
wn_re -Target <abs path>
e`, then remove just the link with `(Get-Item C:
wn_re).Delete()`.
- **Python:** `python -m venv venv; venv\Scripts\pip install pefile capstone`.
  - Run the scripts with `-I`.
  - Don't name a script after a stdlib module. For example, `dis.py` breaks capstone's import.
- **Harness SDL:** the offline harness needs the official SDL2.dll 2.0.8 x64. It's at `data\SDL2.dll`. Run it from `controller_mod`: `build\Release\nwnc_harness.exe re\data\SDL2.dll data`.
