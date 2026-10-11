# winapi_stubs — import libs Windows manquantes du sysroot zig

`zig cc --target x86_64-windows-gnu` apporte un sysroot MinGW complet, mais
deux import libs ne sont pas incluses parce que zig ne fournit pas tous les
fichiers .lib des nouvelles APIs Windows :

- `libsynchronization.a` : exports `Synchronization.dll` (`WaitOnAddress`,
  `WakeByAddressSingle`, `WakeByAddressAll`). Utilisé par Qt6 (mutex/condvar).
- `libruntimeobject.a` : exports `combase.dll` / `RoBase.dll`
  (`RoGetActivationFactory`, `RoActivateInstance`, etc.). Utilisé par Qt6
  pour les WinRT bridges.

Sans ces .a, le link de Qt6 sous zig cc échoue avec
`unable to find static system library 'synchronization' / 'runtimeobject'`.

## Origine

Copiés depuis MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-windows-default-manifest` +
`mingw-w64-ucrt-x86_64-headers`). Ce sont de pures import libs MinGW, pas
du code, juste des stubs vers les DLL système Windows. Architecture
x86_64-w64-mingw32, cohérent avec le target zig.

## Pourquoi versionnés ici

Plutôt que de les copier depuis MSYS2 au moment du `setup_zig.sh` (couplage
à l'install MSYS2 du contributeur), on les commit directement (84 KB total).
Reproductibilité parfaite, plus de dépendance à un layout MSYS2 spécifique.

## Régénération (si bump zig fait évoluer les besoins)

Depuis MSYS2 UCRT64 à jour :

```
cp /c/msys64/ucrt64/lib/libsynchronization.a vendor/winapi_stubs/
cp /c/msys64/ucrt64/lib/libruntimeobject.a   vendor/winapi_stubs/
```

Alternative future via `zig dlltool` (TODO si on veut couper la dette MSYS2 résiduelle) :

```
zig dlltool -d synchronization.def -l libsynchronization.a -m i386:x86-64
```

(nécessite de maintenir un fichier .def avec la liste des exports, ce que les
.a MinGW font déjà pour nous.)
