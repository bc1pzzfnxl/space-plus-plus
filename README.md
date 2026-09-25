# Space++ — Black Hole Simulation Sandbox

A real-time C++20 / OpenGL 4.6 sandbox: ray-traced **Kerr** lensing
(Carter constants, Boyer–Lindquist coordinates, Mino-time RK4; exact
Schwarzschild limit at `a* = 0`), a luminous animated accretion disk
(g-factor physics, Shakura–Sunyaev profile, plunging-region emission),
polar jets, an ergosphere glow, a warped space-time grid, an orbiting
particle swarm (timelike RK4 geodesics, ISCO capture), an orbiting camera,
an ImGui settings panel with the thermodynamics HUD (spec section 5),
retro block rendering and a polish pass (bloom + FXAA).

## Kerr physics

- Geodesics integrate Carter's separated equations in Mino time
  `dτ = dλ/ρ²` with velocity-capped RK4; `ξ`, `η` constants of motion and
  the exact `dφ/dτ = ξ/sin²θ − a + a(r²+a²−aξ)/Δ` law (metric contraction
  verified against `ρ²·g^{μφ}p_μ`).
- Disk emission uses `Ω_K = 1/(r^{3/2}+a)`, the full equatorial
  `g_tφ` fluid 4-velocity (spec §4.1) and the arriving photon's
  `λ = −ξ` in the g-factor; `I ∝ g⁴ I_e`.
- Inner edge follows the Kerr ISCO (spec §2.2, co-rotating family:
  6 M at `a*=0`, 2.32 M at `+0.9`, 8.72 M at `−0.9`).
- Realism toggles (Settings → Realism): plunging-region emission,
  polar jets with advected knots, ergosphere piercing glow (escaped rays
  only), disk turbulence.
- Thermo HUD adds the Kerr horizon `r₊`, the equatorial ergoregion width
  and the Kerr ISCO on top of the Schwarzschild rows.

## Build on Linux

Dependencies (Arch / CachyOS):

```bash
sudo pacman -S --needed cmake gcc sdl2 glew glm mesa
```

On Debian / Ubuntu:

```bash
sudo apt install cmake g++ libsdl2-dev libglew-dev libglm-dev
```

- CMake ≥ 3.20, a C++20 compiler, OpenGL ≥ 4.6 driver.
- Dear ImGui is vendored in `third_party/imgui/` (no `git clone` needed).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/blackhole
```

## Build on Windows

Prerequisites:

- **Visual Studio 2022** with the *Desktop development with C++* workload
- CMake (ships with VS, or `winget install Kitware.CMake`)
- [vcpkg](https://github.com/microsoft/vcpkg):

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Build (SDL2 / GLEW / GLM install automatically via `vcpkg.json`):

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The executable is `build\Release\blackhole.exe`. Copy the DLLs next to it
(vcpkg installs into `build\vcpkg_installed\`; look for `SDL2.dll` and
`glew32.dll` under `bin\` if the path differs):

```bat
copy build\vcpkg_installed\x64-windows\bin\*.dll build\Release\
build\Release\blackhole.exe --pixel --polish --anim
```

### NVIDIA checklist

Run:

```bat
build\Release\blackhole.exe --frames 60 --anim --pixel --polish --screenshot shots\nvidia.bmp
```

1. The first terminal line must read
   `OpenGL 4.6 (Core Profile) ... on NVIDIA GeForce ...`.
   If it shows the CPU/Intel GPU, the laptop is on the iGPU — set
   *NVIDIA Control Panel → Manage 3D settings → High-performance NVIDIA
   processor*, or *Windows Settings → Graphics → blackhole.exe → High
   performance*.
2. The black shadow, disk, blue grid and cyan particles all render.
3. **Settings → Rendering**: move *Pixel size* (immediate retro effect) and
   toggle *Polish*.
4. Resize the window: no black screen (FBOs are recreated).
5. Right-click in the window: launches a particle toward the cursor.

A recent Studio driver is recommended. OpenGL 4.6 is native on every NVIDIA
GPU from Fermi onward on Windows. If the console flashes and closes, launch
from `cmd.exe` to read the error.

## On-screen controls

| Action               | Control                             |
|----------------------|-------------------------------------|
| Orbit                | left mouse drag                     |
| Zoom                 | mouse wheel                         |
| Launch a particle    | right click (toward the cursor)     |
| Camera views         | **Side / 45 deg / Top** buttons     |
| Simulation           | **Play / Pause / Step / Reset**, speed x0.25–x4 |
| Swarm lab            | population, top-up interval, spawn radius, eccentricity, inclination |
| Disk / mass          | **Settings** (left) and **Thermodynamics** (right) panels |
| Spin `a*`            | **Settings → Accretion disk** (−0.95…+0.95, ISCO edge follows) |
| Realism              | **Settings → Realism**: plunge, jets, ergosphere, turbulence |
| Rendering            | **Rendering** section: frame time, pixel size (1–8 px), polish + bloom |
| Thermodynamics HUD   | `r_s`, photon sphere, `r₊`, ergoregion, ISCO, `T_Hawking`, entropy, `dM/dt`, evaporation time |
| Quit                 | `Esc` or close the window           |

The disk shimmers continuously (azimuthal texture sheared by Keplerian
rotation — the g⁴ physics is unchanged). The swarm is 70 % bound orbits
(relativistic precession) and 30 % plungers; a captured particle is recycled
so the population stays stable.

### Command-line options

| Option               | Effect                                                   |
|----------------------|----------------------------------------------------------|
| `--frames N`         | exit automatically after N frames                        |
| `--screenshot f.bmp` | save the last frame (requires `--frames`)                |
| `--az <degrees>`     | starting camera azimuth (view testing)                   |
| `--orbit <rad/s>`    | camera auto-orbit speed (`0` = static)                   |
| `--anim`             | start the swarm clock (particles move)                   |
| `--spawn N`          | spawn N extra particles at startup                       |
| `--click fx fy`      | launch a particle toward a viewport point (fractions 0–1)|
| `--pixel [N]`        | N×N pixel block rendering (default 4, `1` = native)      |
| `--polish`           | light bloom + FXAA; FXAA turns off automatically in pixel mode |
| `--spin <a*>`        | initial dimensionless spin (−0.95…+0.95)                 |
| `--mass <M_sun>`     | initial mass (grid depth ∝ M^0.25, Kepler clock ∝ 1/√M)  |

Example — polished retro render after 120 frames:

```bash
./build/blackhole --pixel --polish --anim --frames 120 --screenshot shots/capture.bmp
```

Screenshots convert to PNG with ImageMagick:

```bash
magick shots/capture.bmp shots/capture.png
```

## Project layout

```
CMakeLists.txt        build (C++20, SDL2/GLEW/OpenGL, static imgui lib)
src/main.cpp          window, render passes (raytrace/grid/post), swarm,
                      camera orbit, ImGui panel, HUD §5, Kerr ISCO helper
src/shaders.hpp       embedded GLSL (Kerr geodesics, disk emission, jets,
                      ergosphere, grid, blur/FXAA)
vcpkg.json            Windows dependencies (sdl2, glew, glm)
third_party/imgui/    vendored Dear ImGui
shots/                verification captures
```
