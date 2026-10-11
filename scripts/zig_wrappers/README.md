# Wrappers zig pour owl-build (Phase B)

Ces wrappers permettent à CMake d'invoquer `zig cc` / `zig c++` / `zig ar` /
`zig ranlib` / windres comme s'il s'agissait de binaires séparés. CMake
ne supporte pas les commandes multi-mots pour `CMAKE_AR` / `CMAKE_RANLIB`,
d'où le besoin de wrappers.

## Variables d'environnement

- `OWL_ZIG` : chemin vers le binaire `zig` à invoquer
  (defaut : `<repo>/vendor/zig/<host>/zig` resolu via `setup_zig.sh`)

## Liste des wrappers

- `zig-cc`     : `zig cc` (compile C)
- `zig-c++`    : `zig c++` (compile C++)
- `zig-ar`     : `zig ar` (archiver)
- `zig-ranlib` : `zig ranlib`
- `windres`    : wrapper qui appelle `zig rc` (Windows resource compiler)

## Particularités

Les wrappers `zig-cc` / `zig-c++` traduisent automatiquement deux conventions
GNU ld → lld :

1. `-Wl,-subsystem,X` → `-Wl,--subsystem,X` (lld veut 2 dashes)
2. Injection automatique de `-L<repo>/vendor/zig/winapi_stubs/` pour les
   import libs Windows (`synchronization`, `runtimeobject`) absentes du
   sysroot zig.
