# Space++ — Simulateur de trou noir (sandbox)

Sandbox de simulation en C++20 / OpenGL 4.6 : lentilles de Schwarzschild tracées
au rayon (RK4), disque d'accrétion lumineux et animé, grille d'espace-temps
déformée, essaim de particules en orbite (géodésiques timelike RK4, capture
ISCO), caméra orbite, panneau de paramètres ImGui (HUD thermodynamique §5 du
cahier des charges), rendu rétro par blocs et polish (bloom + FXAA) — tout en
temps réel, comme un banc d'essai physique.

## Dépendances (CachyOS / Arch)

```bash
sudo pacman -S --needed cmake gcc sdl2 glew glm mesa
```

- CMake ≥ 3.16, g++ (C++20), SDL2, GLEW, GLM, pilote OpenGL ≥ 4.6.
- Dear ImGui est déjà vendorisé dans `third_party/imgui/` (aucun `git clone` nécessaire).

## Windows / NVIDIA

Build portable (CMake + vcpkg) et binaires prêts à l'emploi : voir
[BUILD_WINDOWS.md](BUILD_WINDOWS.md). Chaque push compile aussi sous
`windows-latest` et `ubuntu-24.04` via GitHub Actions (artefact zip dans
l'onglet Actions).

## Construction

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Lancer l'application

```bash
./build/blackhole
```

### Commandes à l'écran

| Action              | Contrôle                          |
|---------------------|-----------------------------------|
| Orbiter             | glisser avec le bouton gauche     |
| Zoom                | molette                           |
| Lancer une particule | clic droit (direction de la souris) |
| Vues de caméra      | boutons **Side / 45 deg / Top**   |
| Simulation          | **Play / Pause / Step / Reset**, vitesse x0.25–x4 |
| Essaim (Swarm lab)  | population, top-up, rayon, excentricité, inclinaison |
| Réglages disque/masse | panneaux **Settings** (gauche) et **Thermodynamics** (droite) |
| Rendu               | section **Rendering** : pixel size (1–8 px), polish + bloom |
| HUD thermodynamique | `r_s`, sphère de photons, ISCO, `T_Hawking`, entropie, `dM/dt`, temps d'évaporation |
| Quitter             | `Échap` ou fermer la fenêtre      |

Le disque scintille en continu (texture azimutale cisaillée par la rotation
Keplerienne — la physique g⁴ est inchangée). L'essaim est peuplé à 70 %
d'orbites liées (précession relativiste) et 30 % de plongeuses ; une
particule capturée est recyclée pour en garder la population stable.

### Options en ligne de commande

| Option                | Effet                                              |
|-----------------------|----------------------------------------------------|
| `--frames N`          | quitte automatiquement après N images              |
| `--screenshot f.bmp`  | enregistre la dernière image (nécessite `--frames`) |
| `--az <degrés>`       | azimuth de départ de la caméra (test de vue)       |
| `--orbit <rad/s>`     | vitesse d'orbite auto de la caméra (`0` = fixe)     |
| `--anim`              | démarre l'horloge de l'essaim (particules en mouvement) |
| `--spawn N`           | spawn supplémentaire de N particules au démarrage   |
| `--click fx fy`       | lance une particule vers un point du viewport (fractions 0–1) |
| `--pixel [N]`         | rendu par blocs de N×N px (défaut 4, `1` = natif)  |
| `--polish`            | bloom (léger) + FXAA ; désactivé automatiquement en mode pixel |

Exemple — rendu rétro polishé après 120 images :

```bash
./build/blackhole --pixel --polish --anim --frames 120 --screenshot shots/capture.bmp
```

Les `.bmp` se convertissent en PNG avec ImageMagick :

```bash
magick shots/capture.bmp shots/capture.png
```

## Structure

```
CMakeLists.txt        build (C++20, SDL2/GLEW/OpenGL, lib statique imgui)
src/main.cpp          fenêtre, passes de rendu (raytrace/grille/post), essaim, orbite, panneau ImGui, HUD §5
src/shaders.hpp       GLSL embarqués (géodésiques RK4, disque, grille, blur/FXAA)
third_party/imgui/    Dear ImGui vendorisé
shots/                captures de vérification
```
