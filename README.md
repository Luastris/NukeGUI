# NukeGUI

The RUNTIME (in-game) GUI module for [NukeEngine](https://github.com/Luastris/NukeEngine-Eco).
Ships with games (unlike [NukeImGui](https://github.com/Luastris/NukeImGui), which is
editor-only); vendors its own static ImGui and draws through the renderer's neutral 2D
seam. Optional — skip it if your game draws no GUI.

## Use it

- **Immediate mode** — `nuke::iGUI` (the `gui` service): windows, text, buttons,
  checkboxes, sliders, input text, combo, image, progress bar + style controls.
  Scripts reach it as `gui.*` / `nuke.Gui.*` (Lua) and `Gui.*` (C#).
- **Retained mode** — `nuke::Ui`: string-id widgets with latched events (build once,
  poll clicks), auto-bound to scripting as `nuke.Ui` / `Ui`.
- Script components get an `OnGUI`/`gui(self)` hook called every frame while playing;
  keyboard + clipboard flow through the render seam.

```lua
gui = function(self)
    gui.begin("HUD")
    gui.text("Score: " .. score)
    if gui.button("Restart") then restart() end
    gui.done()
end
```

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild, or
standalone: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`; engine first).
