# Build sur Windows

Deux chemins : télécharger le binaire déjà compilé, ou compiler chez soi.

## Option 1 — Télécharger le zip (recommandé)

1. Onglet **Actions** du dépôt → dernier run vert → artefact
   `space-plus-plus-windows-x64`.
2. Dézipper : `blackhole.exe` + les DLL + `shots/` + cette doc.
3. Lancer depuis l'explorateur ou :

```bat
blackhole.exe --pixel --polish --anim --frames 120 --screenshot shots\capture.bmp
```

## Option 2 — Compiler soi-même (vcpkg + Visual Studio 2022)

Prérequis :

- Visual Studio 2022 avec le workload **Développement desktop C++**
- CMake (inclus avec VS, ou `winget install Kitware.CMake`)
- [vcpkg](https://github.com/microsoft/vcpkg) :

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Build (les deps SDL2/GLEW/GLM s'installent automatiquement via `vcpkg.json`) :

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

L'exécutable est dans `build\Release\blackhole.exe`. Copier à côté les DLL :

```bat
copy vcpkg_installed\x64-windows\bin\*.dll build\Release\
```

## Checklist de test (GPU NVIDIA)

Lancer :

```bat
build\Release\blackhole.exe --frames 60 --anim --pixel --polish --screenshot shots\nvidia.bmp
```

1. **Première ligne du terminal** : doit afficher
   `OpenGL 4.6 (Core Profile) ... on NVIDIA GeForce ...`.
   Si le nom du GPU est celui du processeur/Intel → le PC bascule sur l'iGPU :
   *Panneau de configuration NVIDIA → Gestionnaire 3D → processeur haute
   performance*, ou *Paramètres Windows → Graphiques → blackhole.exe → Hautes
   performances*.
2. L'ombre noire, le disque, la grille bleue et les particules cyan s'affichent.
3. Section **Rendering** du panneau Settings : bouger *Pixel size* (effet
   rétro immédiat) et cocher *Polish*.
4. Redimensionner la fenêtre : pas d'écran noir (les FBO se recréent).
5. Clic droit dans la fenêtre : lance une particule vers le curseur.

Un driver **Studio** récent est recommandé (le driver Game Ready convient
aussi). OpenGL 4.6 est natif sur tout GPU NVIDIA Fermi+ sous Windows.

## Dépannage

- `VCRUNTIME140.dll manquant` → installer *Redistribuable Visual C++* (ou
  lancer depuis une invite VS dev).
- Écran noir au démarrage : vérifier que les DLL sont bien à côté de l'exécutable.
- `--screenshot` crée le dossier automatiquement, séparateurs `/` et `\`
  fonctionnent tous les deux.
