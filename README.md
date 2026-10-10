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

Лупа живёт в фоне, стартует вместе с Windows и сама обновляется: окна и значка
у неё нет, пока не нажато Win+Z.

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

В скриптах не ждите установку через `Start-Process -Wait`: он ждёт и лупу,
которую установщик запускает в конце, то есть вечно. Ждите только сам
установщик:

```powershell
(Start-Process .\ScreenLoupe-Setup.exe '/VERYSILENT /SUPPRESSMSGBOXES' -PassThru).WaitForExit()
```

## Автообновление

Лупа обновляется сама, без окон и вопросов:

1. Через минуту после запуска (обычно после входа в Windows) она сверяет свою
   версию с последним [релизом](https://github.com/talaev-sergey/screen-loupe-win/releases/latest).
   Нет сети — повторяет каждые 15 минут.
2. Если релиз новее, скачивает установщик и сверяет его SHA-256 с
   `checksums.txt` из того же релиза. Не совпало — ничего не ставит и
   пробует при следующем запуске.
3. Ждёт, пока лупа выключена: урок не прервётся.
4. Запускает установщик в тихом режиме. Тот останавливает старую версию,
   ставит новую и запускает её.

Журнал последнего обновления — `%TEMP%\ScreenLoupe-update.log`.

Обновить вручную можно так же: запустите установщик новой версии поверх
старой.

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
- Запись экрана и демонстрация в Zoom, Teams и т. п. затемнение не видят:
  оно исключено из захвата, иначе попало бы в саму лупу. На проекторе и
  мониторе всё видно как обычно.
