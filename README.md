# Emulate

**English** | [한국어](#한국어)

An all-in-one Android emulator for **Game Boy, Game Boy Color, Game Boy Advance and Nintendo DS**, with a Material You interface, touch controls you can rearrange, save states, cheats, and local multiplayer between phones.

| System | Core |
| --- | --- |
| Game Boy / Color / Advance | [mGBA](https://github.com/libretro/mgba) (libretro) |
| Nintendo DS | [melonDS DS](https://github.com/JesseTG/melonds-ds) 1.4.0 (libretro) — runs without a BIOS/firmware dump; uses real ones if you provide them |

The app is a custom libretro frontend (`app/src/main/cpp`) that loads both cores with `dlopen`, plus a Kotlin / Jetpack Compose UI.

## Features

- **ROM library** — import ROMs (also from `.zip`); NDS games show their banner icon and title. Long-press a game for ROM info (header title, game code, region, checksums, CRC32…) and a custom display name that doesn't touch your saves.
- **Touch gamepad** — multi-touch, slide between A/B, haptic feedback. Every button can be moved, resized (50–200%) and hidden, saved separately per system and orientation. Bluetooth/USB gamepads and keyboards work too.
- **Saves** — battery saves auto-saved, 8 save-state slots with screenshot previews, import/export of `.sav` files (including DeSmuME `.dsv` conversion), and full ZIP backup/restore.
- **Cheats** — GameShark, Game Genie, Action Replay, CodeBreaker (depending on system).
- **Video** — OpenGL ES rendering with filters (sharp / pixel-perfect / smooth / LCD grid), refresh-rate matching for 60/120 Hz displays, and an ambient glow that follows the game's edge colors. DraStic-style overlay layout on short screens.
- **Speed** — fast-forward ×1.5–×8 and slow-motion ×0.5/×0.75, with audio kept at the original pitch.
- **NDS extras** — microphone (blow) button, lid open/close, flexible dual-screen layouts, optional real BIOS/firmware, GBA Slot-2 cartridge.
- **Multiplayer over Wi-Fi** — NDS local wireless and GB/GBC/GBA link cable between phones on the same network.

## Requirements

**To run the app**

- Android **8.0 (API 26)** or newer; Android 12+ recommended for dynamic Material You colors
- 64-bit device (**arm64-v8a**; x86_64 is included for emulators)
- Your own ROM files (`.gb`, `.gbc`, `.gba`, `.nds`, or zipped)
- For multiplayer: all devices on the same Wi-Fi network (or one phone's hotspot)

**To build from source**

- Android SDK (compileSdk 36), **NDK 28.2.13676358**, **CMake 3.22.1**
- **JDK 17** or newer (Android Studio's bundled JBR works)
- Git and a Bash shell (Git Bash on Windows)

## Getting Started

### Install & play

1. Install the APK on your device (enable "install unknown apps" if needed).
2. Open the app and tap **+** to import a ROM.
3. Tap a game to start. Open the in-game menu for save states, cheats, controller layout editing, game speed, and link/wireless options.

### Build from source

```bash
git clone <this-repo-url> Emulate
cd Emulate

scripts/fetch-cores.sh      # download pinned core sources into third_party/
scripts/build-cores.sh      # build cores with the NDK into app/src/main/jniLibs/
./gradlew assembleDebug     # APK: app/build/outputs/apk/debug/
```

On Windows, run the two scripts from Git Bash and use `gradlew.bat assembleDebug` in PowerShell. Set `ANDROID_HOME` if your SDK is not in the default location.

### Multiplayer

1. Run the same game on both devices (same Wi-Fi).
2. On one device open the menu → **Wireless** (NDS) or **Link cable…** (GB/GBC/GBA) → **Create room**.
3. On the other device choose **Find room** (or **Join by IP**, port `47000`).
4. Start the in-game link feature as usual.

## Notes & Limitations

- **No games or BIOS files are included.** Use ROMs dumped from cartridges you own.
- **Local network only.** Multiplayer is latency-sensitive and does not work over the internet; public Wi-Fi often blocks device-to-device traffic. Link-cable games with frequent transfers (e.g. Gen 3 Pokémon trading) slow down in proportion to Wi-Fi latency.
- Fast-forward and loading save states are disabled while connected to another device, to keep both sides in sync.
- NDS BIOS/firmware is optional; if you supply them, all three of `bios7.bin`, `bios9.bin` and `firmware.bin` are required.
- Imported ROMs are copied into the app's private storage; uninstalling the app deletes them and their saves — make a **backup** first (⋮ menu → backup).
- Release builds are currently signed with the debug key and are not minified.
- This is an early version (0.1.0); some games may have glitches or performance problems on low-end devices.

## License

The melonDS DS core is **GPLv3**, so an app distributed with it must also follow GPLv3. mGBA is MPL 2.0. Google Sans Flex is under SIL OFL 1.1 (`app/src/main/assets/licenses/`), and the icons are Material Icons (Apache 2.0).
This project is not affiliated with Nintendo. Nintendo, Game Boy and Nintendo DS are trademarks of their respective owners.

---

<a id="한국어"></a>

# Emulate (한국어)

[English](#emulate) | **한국어**

**게임보이, 게임보이 컬러, 게임보이 어드밴스, 닌텐도 DS**를 하나로 묶은 안드로이드 에뮬레이터입니다. Material You 디자인, 마음대로 옮길 수 있는 터치 버튼, 세이브 스테이트, 치트, 폰끼리 하는 로컬 멀티플레이를 지원합니다.

| 기종 | 코어 |
| --- | --- |
| Game Boy / Color / Advance | [mGBA](https://github.com/libretro/mgba) (libretro) |
| Nintendo DS | [melonDS DS](https://github.com/JesseTG/melonds-ds) 1.4.0 (libretro) — BIOS/펌웨어 없이 동작하며, 실기 파일을 넣으면 그것을 사용 |

앱은 두 코어를 `dlopen`으로 불러 쓰는 자체 libretro 프론트엔드(`app/src/main/cpp`)와 Kotlin / Jetpack Compose UI로 구성됩니다.

## 주요 기능

- **ROM 라이브러리** — ROM 가져오기(`.zip` 포함), NDS는 배너 아이콘과 제목 표시. 게임을 길게 누르면 ROM 정보(내부 제목, 게임 코드, 지역, 체크섬, CRC32 등)와 목록에 보일 이름(별칭)을 정할 수 있으며, 세이브에는 영향이 없습니다.
- **터치 게임패드** — 멀티터치, A/B 사이 미끄러지듯 누르기, 진동 피드백. 모든 버튼을 옮기고 크기(50~200%)를 바꾸고 숨길 수 있으며, 기종과 가로/세로 화면마다 따로 저장됩니다. 블루투스/USB 게임패드와 키보드도 지원합니다.
- **세이브** — 배터리 세이브 자동 저장, 화면 미리보기가 있는 세이브 스테이트 8칸, `.sav` 가져오기/내보내기(DeSmuME `.dsv` 자동 변환), 전체 ZIP 백업/복원.
- **치트** — 기종에 따라 GameShark, Game Genie, Action Replay, CodeBreaker 코드 지원.
- **화면 출력** — OpenGL ES, 필터(선명하게 / 원본 픽셀 / 부드럽게 / LCD 격자), 60/120Hz 주사율 맞춤, 게임 화면 가장자리 색이 배경으로 번지는 앰비언트 효과. 화면이 짧은 폰에서는 DraStic 방식으로 버튼을 화면 위에 겹쳐 표시합니다.
- **게임 속도** — 빨리 감기 ×1.5~×8, 느리게 ×0.5/×0.75. 소리는 원래 음높이를 유지합니다.
- **NDS 기능** — 마이크(바람 불기) 버튼, 덮개 열기/닫기, 다양한 화면 배치, 실기 BIOS/펌웨어(선택), GBA 슬롯(Slot-2).
- **Wi-Fi 멀티플레이** — 같은 네트워크의 폰끼리 NDS 무선 통신, GB/GBC/GBA 통신 케이블 연결.

## 요구 사양

**앱 실행**

- Android **8.0(API 26)** 이상, 동적 Material You 색상은 Android 12 이상 권장
- 64비트 기기(**arm64-v8a**, 에뮬레이터용으로 x86_64도 포함)
- 직접 가지고 있는 ROM 파일(`.gb`, `.gbc`, `.gba`, `.nds` 또는 zip)
- 멀티플레이: 모든 기기가 같은 Wi-Fi(또는 한쪽 폰의 핫스팟)에 연결

**소스에서 빌드**

- Android SDK(compileSdk 36), **NDK 28.2.13676358**, **CMake 3.22.1**
- **JDK 17** 이상(Android Studio 내장 JBR 가능)
- Git과 Bash 셸(Windows는 Git Bash)

## 시작하기

### 설치 및 실행

1. APK를 기기에 설치합니다(필요하면 "알 수 없는 앱 설치"를 허용).
2. 앱을 열고 **+** 버튼으로 ROM을 가져옵니다.
3. 게임을 눌러 시작합니다. 게임 메뉴에서 세이브 스테이트, 치트, 터치 버튼 배치 편집, 게임 속도, 통신 기능을 쓸 수 있습니다.

### 소스에서 빌드

```bash
git clone <이 저장소 주소> Emulate
cd Emulate

scripts/fetch-cores.sh      # 고정된 커밋의 코어 소스를 third_party/에 받기
scripts/build-cores.sh      # NDK로 코어를 빌드해 app/src/main/jniLibs/에 넣기
./gradlew assembleDebug     # APK: app/build/outputs/apk/debug/
```

Windows에서는 두 스크립트를 Git Bash에서 실행하고, PowerShell에서 `gradlew.bat assembleDebug`를 사용하세요. SDK가 기본 위치에 없으면 `ANDROID_HOME`을 지정합니다.

### 멀티플레이

1. 두 기기에서 같은 게임을 실행합니다(같은 Wi-Fi).
2. 한 기기에서 메뉴 → **무선 통신**(NDS) 또는 **통신 케이블…**(GB/GBC/GBA) → **방 만들기**.
3. 다른 기기에서 **방 찾기**(또는 **IP로 참가**, 포트 `47000`)로 참가합니다.
4. 게임 안에서 평소처럼 통신 기능을 시작합니다.

## 주의사항

- **게임과 BIOS 파일은 포함되어 있지 않습니다.** 직접 소유한 카트리지에서 추출한 ROM만 사용하세요.
- **같은 네트워크 안에서만 동작합니다.** 지연에 민감해 인터넷 너머로는 동작하지 않으며, 공용 Wi-Fi는 기기 간 통신을 막는 경우가 많습니다. 전송이 잦은 통신 케이블 게임(예: 3세대 포켓몬 교환)은 Wi-Fi 지연에 비례해 느려집니다.
- 다른 기기와 연결된 동안에는 동기화를 위해 빨리 감기와 상태 불러오기가 막힙니다.
- NDS BIOS/펌웨어는 선택 사항이며, 넣는다면 `bios7.bin`, `bios9.bin`, `firmware.bin` 세 개가 모두 필요합니다.
- 가져온 ROM은 앱 전용 저장소로 복사되므로, 앱을 삭제하면 ROM과 세이브도 함께 지워집니다. 삭제 전에 **백업**(⋮ 메뉴 → 백업)을 하세요.
- 릴리스 빌드는 현재 디버그 키로 서명되며 코드 축소가 적용되지 않습니다.
- 아직 초기 버전(0.1.0)이라 일부 게임에서 문제가 있거나 저사양 기기에서 느릴 수 있습니다.

## 라이선스

melonDS DS 코어가 **GPLv3**이므로, 이 코어를 포함해 배포하는 앱도 GPLv3을 따라야 합니다. mGBA는 MPL 2.0, Google Sans Flex는 SIL OFL 1.1(`app/src/main/assets/licenses/`), 아이콘은 Material Icons(Apache 2.0)입니다.
이 프로젝트는 Nintendo와 관련이 없으며, Nintendo, Game Boy, Nintendo DS는 각 소유자의 상표입니다.
