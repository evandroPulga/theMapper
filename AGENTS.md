# Project Instructions

## Blender Integration
Blender is running locally with the MCP socket bridge listening on `localhost:9876`.
- For any Blender operations, execute Python (`bpy`) directly using:
  ```powershell
  powershell -ExecutionPolicy Bypass -File "tools\blender_exec.ps1" -Code "<bpy_code>"
  ```
- See detailed instructions in [.agents/rules/blender.md](file:///C:/Workspace/Unreal%20Projects/TheMaper/.agents/rules/blender.md).
