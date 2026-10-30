# Blender MCP Integration Rule

## Overview
Blender is running locally with the official Blender MCP server add-on enabled, listening on `localhost:9876`.

## How to Interact with Blender
Whenever the user asks to inspect, manipulate, export, or generate 3D assets, meshes, materials, animations, or scene data in Blender:
- Do NOT ask the user if Blender is open or accessible.
- Execute Python (`bpy`) scripts in Blender directly using the workspace helper script:
  ```powershell
  powershell -ExecutionPolicy Bypass -File "tools\blender_exec.ps1" -Code "<python_bpy_code>"
  ```
  Or for multi-line / complex scripts, write the script to a `.py` file and run:
  ```powershell
  powershell -ExecutionPolicy Bypass -File "tools\blender_exec.ps1" -File "<path_to_script.py>"
  ```

## Protocol & Conventions
- Code runs in Blender's main thread via Blender's MCP socket bridge (`localhost:9876`).
- Return values should be assigned to `result = {...}` (a dictionary) inside the Python script to be returned as JSON.
- Standard stdout (`print(...)`) is captured and returned in the JSON response under `"stdout"`.
