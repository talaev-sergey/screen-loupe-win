# Screen Loupe для Windows 11

[![build](https://github.com/talaev-sergey/screen-loupe-win/actions/workflows/build.yml/badge.svg)](https://github.com/talaev-sergey/screen-loupe-win/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/talaev-sergey/screen-loupe-win)](https://github.com/talaev-sergey/screen-loupe-win/releases/latest)
[![downloads](https://img.shields.io/github/downloads/talaev-sergey/screen-loupe-win/total)](https://github.com/talaev-sergey/screen-loupe-win/releases)
[![Windows 11](https://img.shields.io/badge/Windows-11-0078D4?logo=windows11&logoColor=white)](#установка)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](src/main.cpp)

Экранная лупа для урока на проекторе. Затемняет весь экран, кроме круга у
курсора. Круг ходит за курсором, а колесо мыши увеличивает картинку в нём.

- **Win+Z** включает и выключает лупу.
- **Esc** выключает лупу.
- **Колесо мыши** приближает и отдаляет картинку в круге: от 1× (просто светлое
  пятно) до 8×.

Лупа живёт в фоне и стартует вместе с Windows: окна и значка у неё нет, пока не
нажато Win+Z.

## Установка

[![Скачать установщик](https://img.shields.io/badge/Скачать-ScreenLoupe--Setup.exe-2ea44f?style=for-the-badge&logo=windows11&logoColor=white)](https://github.com/talaev-sergey/screen-loupe-win/releases/latest/download/ScreenLoupe-Setup.exe)

1. Скачайте `ScreenLoupe-Setup.exe` по кнопке выше или со страницы
   [релизов](https://github.com/talaev-sergey/screen-loupe-win/releases/latest).
2. Запустите его. Windows может показать синее окно «Система Windows защитила
   ваш компьютер»: установщик не подписан. Нажмите **Подробнее** →
   **Выполнить в любом случае**.
3. Нажмите **Установить**, затем **Завершить**. Права администратора не нужны.

Установщик кладёт программу в `%LOCALAPPDATA%\Programs\Screen Loupe`, добавляет
её в автозагрузку текущего пользователя и сразу запускает в фоне. Можно жать
Win+Z.

**Проверка файла.** Рядом с установщиком в релизе лежит `checksums.txt`. Хеш
должен совпасть:

```powershell
(Get-FileHash .\ScreenLoupe-Setup.exe -Algorithm SHA256).Hash
```

**Тихая установка** (на несколько машин, без окон):

```powershell
.\ScreenLoupe-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES
```

**Обновление.** Запустите установщик новой версии поверх старой: он сам
остановит работающую лупу и запустит новую.

## Удаление

**Параметры → Приложения → Установленные приложения → Screen Loupe →
Удалить.** Деинсталлятор останавливает лупу и убирает её из автозагрузки.

Чтобы лупа просто не стартовала с Windows, не удаляя её, выключите её в
**Диспетчере задач → Автозагрузка приложений**.

## Ограничения

- Win+Z в Windows 11 открывает «Макеты прикрепления». Пока лупа запущена, это
  сочетание достаётся ей. Макеты остаются доступны при наведении на кнопку
  «Развернуть» у окна.
- Пока лупа включена, колесо мыши управляет увеличением и страницу не
  прокручивает. Нужно прокрутить — нажмите Esc.
- Если активно окно программы, запущенной от имени администратора, Windows не
  отдаёт лупе клавиатуру и колесо. Тогда щёлкните по любому другому окну.
- Размер круга задан в пикселях и от масштаба экрана не зависит.

## Настройка

Параметры заданы константами в начале [`src/main.cpp`](src/main.cpp). После
правки нужна пересборка.

| Параметр       | Значение | Что делает                              |
|----------------|----------|-----------------------------------------|
| `RADIUS`       | `130`    | радиус круга, px                        |
| `DIM_ALPHA`    | `150`    | сила затемнения снаружи (0–255)         |
| `FRAME_MS`     | `10`     | период обновления (~64 кадра в секунду) |
| `BORDER`       | `2`      | толщина рамки круга, px                 |
| `BORDER_COLOR` | серый    | цвет рамки                              |
| `ZOOM_STEP`    | `0.5`    | прирост увеличения на щелчок колеса     |
| `ZOOM_MAX`     | `8.0`    | максимальное увеличение                 |

## Как устроено

- Затемнение — неподвижное полупрозрачное окно на все мониторы. Оно не
  пропускает клики и не забирает фокус.
- Круг — маленькое окно поверх затемнения с элементом
  [Magnification API](https://learn.microsoft.com/windows/win32/api/_magapi/),
  тем же движком, что у встроенной лупы Windows. Он показывает экран без окон
  самой программы, поэтому при 1× круг выглядит как дырка в затемнении, а при
  большем увеличении работает как лупа. Центр круга всегда под курсором, так что
  клик попадает туда, куда показывает увеличенная картинка.
- Win+Z, Esc и колесо ловят low-level хуки в отдельном потоке. Колесо
  перехватывается только пока лупа включена.

Linux-версия под X11 лупу не умела: под picom картинка захватывалась вместе с
затемнением. В Windows композитор DWM отдаёт экран без наших окон, поэтому здесь
увеличение работает.

## Сборка из исходников

Нужны Visual Studio 2022 (рабочая нагрузка «Разработка классических приложений
на C++»), CMake 3.20+ и [Inno Setup 6](https://jrsoftware.org/isinfo.php).

```powershell
cmake -B build -A x64
cmake --build build --config Release
iscc /DAppVersion=0.1.0 installer\ScreenLoupe.iss
```

Программа — `build\Release\ScreenLoupe.exe`, установщик —
`dist\ScreenLoupe-Setup.exe`. CRT слинкован статически, поэтому VC++
Redistributable на машине не нужен.

Релиз собирает CI: тег `vX.Y.Z` → GitHub Actions собирает установщик, считает
`checksums.txt` и публикует релиз.
