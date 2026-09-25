# Homebrew Xbox 360 — XboxHello + Plex360

Deux apps `.xex` pour console **RGH/JTAG** (ou devkit), toolchain standalone :

- **Plex360** — client Plex natif : bibliothèques, grille de posters, fiches
  détaillées, visionneuse photo. HTTP sockets + XML + JPEG.
- **XboxHello** — démo D3D9/XInput (Hello World avancé).

## Build — aucun prérequis à installer

Le SDK 21256.3 est déjà extrait dans `sdk\extracted\XDK` et le toolchain
(compilateur Xenon PPC, linker, imagexex) s'utilise directement en ligne de
commande. **VS2010 n'est PAS nécessaire.**

```
build.bat            → build\Release\bin\Plex360.xex
build.bat XboxHello  → build\Release\bin\XboxHello.xex
```

## Plex360 — configuration

Copie `build\Release\bin\Plex360.xex` **et** `build\Release\bin\config.ini`
(copié automatiquement par le build) sur la console. Édite `config.ini` :

```ini
server=192.168.1.10   ; IP de ton serveur Plex
port=32400
;token=               ; optionnel si le LAN est en "allowed without auth"
```

Astuce token : dans Plex Web → Settings → Network → *"List of IP addresses and
networks that are allowed without auth"* ajoute `192.168.1.0/24` (adapte ton
sous-réseau) → pas besoin de token.

## Plex360 — contrôles

| Input            | Action                                        |
|------------------|-----------------------------------------------|
| D-PAD            | Naviguer sections / grille                    |
| A                | Ouvrir section/item, photo plein écran        |
| B                | Retour                                        |
| LB / RB          | Sauts de 10 items dans la grille              |
| BACK + START     | Quitter (retour dashboard)                    |

Écrans : bibliothèques → grille de posters (chargés en tâche de fond via
`/photo/:/transcode`) → détail (synopsis, durée, année) → visionneuse photo.
La lecture vidéo n'est pas encore implémentée (voir roadmap).

## XboxHello — contrôles

| Input            | Action                          |
|------------------|---------------------------------|
| Stick gauche     | Déplacer le curseur             |
| A                | Rumble + compteur d'appuis      |
| BACK + START     | Quitter (retour au dashboard)   |

## Contrôles

| Input            | Action                          |
|------------------|---------------------------------|
| Stick gauche     | Déplacer le curseur             |
| A                | Rumble + compteur d'appuis      |
| BACK + START     | Quitter (retour au dashboard)   |

## Déploiement sur la console (RGH/JTAG)

- **USB** : copier `XboxHello.xex` sur une clé FAT32 → le lancer depuis
  Aurora / FreestyleDash / XeXMenu (navigateur de fichiers → exécuter le .xex).
- **LAN** : via un serveur FTP/FSD/Aurora actif sur la console, ou Xbox 360
  Neighborhood si tu installes un jour le setup complet (VS2010 + SDK).

## Tester sans console

**Xenia Canary** exécute les `.xex` homebrew : glisser-déposer
`XboxHello.xex` sur `xenia_canary.exe`.

## Structure

```
XboxHello.sln              Solution VS2010 (optionnel, si VS2010+XDK installes)
build.bat                  Build standalone generique : build.bat [Projet]
sdk/
  XBOX360_SDK_21256.3.exe  Installeur source (archive.org, sha256 verifie)
  extracted/XDK/           SDK extrait : bin\win32 (outils), include\xbox,
                           lib\xbox, TechPreview\Jul12Compiler (headers CRT)
XboxHello/
  XboxHello.vcxproj        Projet plateforme "Xbox 360" (toolset 2010-01)
  src/
    main.cpp               App : D3D9 + shaders + XInput + HUD (~450 lignes)
    font8x8.h              Font bitmap 8x8 embarquee (public domain)
Plex360/
  Plex360.vcxproj          Projet Xbox 360
  config.ini               Exemple de config serveur
  src/
    main.cpp               Machine a ecrans + worker thread + queue reseau
    net.{h,cpp}            HTTP GET sur sockets BSD (XNetStartup, DNS, chunked)
    plex.{h,cpp}           Chemins API Plex + parsing des reponses XML
    xmlmini.{h,cpp}        Extracteur d'elements XML (tags + attributs)
    renderer.{h,cpp}       Quads/texte/images D3D9 (3 batches, VB unique)
    font8x8.h              Font bitmap
```

## Notes techniques

- **Pas de fixed-function pipeline** sur Xenon : tout passe par des shaders
  SM3 compilés à l'exécution via `D3DXCompileShader`.
- `D3DRS_HALFPIXELOFFSET` activé pour l'alignement pixel des quads 2D.
- `D3DUSAGE_DYNAMIC` / `D3DLOCK_DISCARD` n'existent pas sur Xenon.
- Infos système via API documentées : `XGetVideoMode`, `XGetVideoCapabilities`,
  `XGetGameRegion`, `XGetLanguage`, `GetTickCount`.
- Headers CRT publics : `XDK\TechPreview\Jul12Compiler\include\xbox`
  (`XDK\Source\crt` contient les headers *internes* au CRT, ne pas utiliser).
- Le framework ATG (`XDK\Source\Samples\Common`) est volontairement évité.
- Plex360 : les résultats réseau arrivent sur une queue (worker thread → main) ;
  chaque requête de liste porte un ticket pour ignorer les réponses périmées.
  Posters via l'endpoint transcode de Plex (pas besoin du full-res).

## Si tu installes quand même VS2010 + SDK officiellement

1. `en_visual_studio_2010_ultimate_x86_dvd_509116.iso` (gratuit sur
   my.visualstudio.com, sha256 `602926c8...bff98`) — AVANT le SDK.
2. `XBOX360_SDK_21256.3.exe` (sha256 `efec946c...dc4bc7` — le fichier dans
   `sdk/` est déjà ce binaire vérifié).
3. Ouvre `XboxHello.sln`, config `Release` ou `Release_LTCG`, Build.
