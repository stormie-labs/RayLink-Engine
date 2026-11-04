# Безоконный сервер карты: аудит (Ф3a, Task 5)

Ключ `-coop_mapserver <level> <gvid> <port> <token>`. Флаги: `g_coop_headless = true`, `g_dedicated_server = true`
(`x_ray.cpp`, `coop_headless_read_key`, первая строка `WinMain_impl` после `Debug._initialize`). Разбор аргументов ключа
в xrGame делает T9. Номера строк: состояние этой ветки на момент коммита с этим файлом.

## 1. Рендер: что выбрано и почему

В выпускаемую конфигурацию `DX11-AVX` линкуется **только R4**: `ReleaseR4-AVX`, `STATIC_RENDERER_R4`,
`EngineAPI.cpp:49-88` запрещает два рендера сразу, `xrRender_R4.lib` статически вшит. R1 и `d3d9.dll` из плана в этом exe нет.

**Выбор: R4 на устройстве D3D11 WARP, без цепочки подкачки, цели 64×64, окно не показывается.**

| Вариант | Итог | Причина |
|---|---|---|
| Нулевой рендер (`IRender`-заглушка) | нет | рендер нельзя выкинуть: кости, модели, секторы живут в xrRender, а `::Render`, `getTarget()`, `RCache` дёргает игра. Заглушка `ID3D11Device` + контекст это 150+ методов COM ради того, что WARP даёт даром |
| Аппаратное D3D11 | нет | драйвер видеокарты в процессе (его потоки и 100+ МБ, кэш шейдеров), VRAM той карты, на которой хост играет, потеря устройства заодно с игрой |
| **WARP** | **да** | часть Windows, драйвера нет, VRAM нет, рисовать нечего, поэтому скорость не важна. Память устройства в `PrivateUsage`, её и режем ниже |

Что сделано (`dx10HW.cpp`, `CHW::CreateHeadless`):
- `D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, ...)`, уровень 11_0;
- цепочки подкачки нет: `pBaseRT`/`pBaseZB` на обычных текстурах. Поэтому `CHW::Reset` и `updateWindowProps` не работают, а именно
  они показывают окно (`SWP_SHOWWINDOW`) и забирают фокус (`SetForegroundWindow`);
- `Device.Begin()` всегда `FALSE`: нет `seqRender`, нет `End`, нет `Present`, нет опроса состояния устройства;
- вендор в `Caps` = Microsoft, `iGPUNum = 1`: NVAPI не грузится;
- `CreateRendererList` не пробует GPU (`SupportsDX11Rendering` грузит драйвер ради проверки), список из одного `renderer_r4`.

Цели рендера и память:
- все цели `CRenderTarget` идут от 64×64, `o.smapsize = 64`, MSAA, HDR10 и 3D-дым выключены принудительно, что бы ни лежало в `user.ltx`;
- текстуры не грузятся, уровень (`level.geom`, визуалы, детали) не грузится;
- вершинные и индексные буферы моделей: заглушка 16 байт (`dx10BufferUtils::CreateBuffer`). Объект D3D нужен (`CreateGeom` падает на
  нулевом буфере), CPU-копии вершин для пикинга остаются в `CSkeletonX`;
- `CRenderTarget` сохранён: `getTarget()` читают `CameraManager.cpp:528,552` и `ui_base.cpp`.

Кадр: 30 в секунду, `Sleep` в конце `on_idle`, `g_svDedicateServerUpdateReate = 30`. Цикл ECO_RENDER выключен, он крутится вхолостую.
Windows 11 режет таймер у процесса без видимого окна (15,6 мс вместо 1, кадров вышло бы ~22), поэтому
`SetProcessInformation(ProcessPowerThrottling)` снимает и это, и EcoQoS.

## 2. Что в плане не совпало с кодом

Всё найдено чтением кода, в живом запуске не проверялось.

| Что | Факт | Сделано |
|---|---|---|
| `HW.cpp:125,208` | в R4 не компилируется, там `xrRenderDX10/dx10HW.cpp` без единой проверки флага | `CreateHeadless` |
| Текстуры, `SH_Texture.cpp:247` | в R4 не компилируется. В `dx10SH_Texture.cpp` проверки нет, текстуры грузились бы все | проверка в `CTexture::Load` |
| `r4_loader.cpp` | `LoadSectors`: `V->vis.box` при `m_root == 0`, падение на каждой карте | внешний сектор = первый |
| `FluidManager`, 3D-дым | `shader->E[i]` при нулевом шейдере, падение в `CRender::create` | `R3FLAG_VOLUMETRIC_SMOKE` снят |
| `xr_effgamma.cpp` | `HW.m_pSwapChain->GetContainingOutput`: падение, а с цепочкой ещё и гамма монитора игрока | проверка цепочки |
| «оставить 138 мест» | в xrGame `g_dedicated_server` выключает почти всю одиночную игру: ИИ, Lua, логику объектов, GC Lua. Опасных 52 места из 138 плюс 2 в xrServerEntities | §5 |
| «ворон» (crow) | `UpdateCL` каждый кадр получают только объекты, которые видел рендер. Без рендера все NPC шли бы с частотой планировщика | `xr_object_list.cpp` |
| `b_is_Active` | скрытое окно никогда не активно, `dwTimeContinual` стоял бы на нуле | `TRUE` в `Run()`, `OnWM_Activate` выключен |
| `g_bootComplete` | ставит только экран загрузки, которого нет | `LoadEnd` |
| Классы `*_s` | `object_factory_register.cpp` при dedicated регистрирует их вместо скриптов, с Lua это двойная регистрация clsid | исключено |
| `-nosound` | NPC оглохнут, см. §7 | по заданию, риск |

## 3. `#ifdef DEDICATED_SERVER` в xrEngine и Layers

Макрос нигде не определён (нет в `vcxproj`, `props`, исходниках), поэтому `#ifdef` мёртв, `#ifndef` всегда живой.

| Файл:строка | Что под ним | Решение | Причина |
|---|---|---|---|
| `device.cpp:79` `Begin` | опрос устройства, `m_pRender->Begin` | **флаг** | `return FALSE`, это единственная точка отказа от рендера |
| `device.cpp:123` `End` | прекеш, `Present` | оставить | после `Begin() == FALSE` не вызывается |
| `device.cpp:188` `PreCache` | `amount = 0` | **флаг** | `dwPrecacheFrame` считает `End`, иначе «прекеш» никогда не кончится |
| бывш. `device.cpp:357, 559` | кадр сервера | **флаг** | теперь рантайм: 30 кадров, замер работы кадра |
| `device.cpp:529` | `seqRender`, `Show` | оставить | закрыт `Begin` |
| `device.cpp:753` `Pause` | тело паузы | **флаг** | Mk не ставит игру на паузу |
| `device.cpp:895` | `ShowCursor`, клип при активации | **флаг** | весь `OnWM_Activate` выключен |
| `Device_create.cpp:103` | внутри `/* */` | оставить | мёртвый комментарий |
| `Device_destroy.cpp:122, 198` | курсор после `Reset` | оставить | `Reset` и `ChangeOutputMonitor` выходят сразу |
| `EngineAPI.cpp:122, 222`, `EngineAPI.h:53` | загрузка рендера | оставить | рендер нужен, R4 подключается как обычно |
| `EngineAPI.cpp:315` `CreateRendererList` | ветка `renderer_r1` | **флаг** | ветка для `renderer_r4`, без пробы GPU |
| `IGame_Level.cpp:150` | `seqRender.Add` | оставить | список никто не обходит |
| `IGame_Level.cpp:171` `OnRender` | `Render->Calculate/Render` | оставить | не вызывается |
| `Rain.cpp:187` `OnFrame` | ранний выход | **флаг** | плюс `UpdateItems`, там лучи по геометрии |
| `Stats.cpp:497` | шрифт статистики | оставить | объект без текстуры, не рисуется |
| `xr_ioc_cmd.cpp:869, 920, 1207, 1292` | `snd_device`, `vid_monitor` | оставить | команды обязаны быть: они в `user.ltx`. С `-nosound` стоково безопасно |
| `x_ray.cpp:103` | хеш exe | оставить | не мешает |
| `x_ray.cpp:244` `InitConsole` | `CTextConsole` | оставить | он открывает окно консоли, нужен `CConsole`, он рисуется только в `seqRender` |
| `x_ray.cpp:591, 599, 714` | список мониторов | оставить | только перечисление |
| `x_ray.cpp:977` | пусто | оставить | |
| `x_ray.cpp:1064` | `Debug._initialize(true)` | оставить | параметр нужен только BugTrap, его в сборке нет. Режим задаёт `set_silent_fatal` |
| `x_ray.cpp:1098-1138` | мьютекс | **флаг** | второй процесс из той же папки иначе выходит с кодом 1 |
| `x_ray.cpp:1246` | `damn_keys_filter` | **флаг** | конструктор выходит сразу: залипание и заставка принадлежат рабочему столу пользователя |
| `x_ray.cpp:1300` | выбор рендера | **флаг** | `renderer_r4`, не из `user.ltx` |
| `x_ray.cpp:1337` | закрытие мьютекса | **флаг** | только если он есть |
| `x_ray.cpp:1647` `LoadBegin` | шрифт, геометрия загрузки | оставить | не рисуется, `LoadDraw` выходит на `Begin` |
| `object_factory*.cpp/h`, `no_single.h`, `dedicated_server_only.h` | `DEDICATED_SERVER_ONLY` | оставить | другой макрос, тоже не определён |
| `Layers/xrRender/*`: `//#ifndef DEDICATED_SERVER` | закомментированы | нечего | рядом уже рантайм-проверка, см. §4 |

## 4. `g_dedicated_server` вне xrGame

| Файл:строка | Что пропускается | Решение |
|---|---|---|
| `x_ray.cpp` `LoadDraw` | рисование загрузки | мёртво: `Device.Begin()` выходит раньше |
| `XR_IOConsole.cpp:381, 802` | фон консоли, `Hide` | годится |
| `Stats.cpp:195` | вывод статистики | годится, не вызывается |
| `IGame_Level.cpp:149` | `IR_Capture` | годится, это и есть «ввод не захватывается» |
| `IGame_Level.cpp:183` | `Render->Render` | годится, не вызывается |
| `ParticlesAsyncManager.cpp:76` | поток частиц | годится |
| `dxRenderDeviceRender.cpp:162` | `DUImpl`, wire-шейдеры | годится |
| `ResourceManager.cpp:284, 310, 329` | шейдеры, `Create` возвращает NULL | годится, но код R4 рассчитан не везде: см. §2 |
| `R_Backend_Runtime.cpp:20, 44` | кадр бэкенда | годится, не вызывается |
| `r__sector.cpp:277` | корень сектора | годится, `r4_loader.cpp` поправлен |
| `r4_loader.cpp:60` | `level.geom`, визуалы, детали | годится |
| `stats_manager.cpp` (11 мест) | счётчики памяти буферов | годится |
| `xrRender_console.cpp:720` | скриншот | годится |
| `HW.cpp`, `SH_Texture.cpp`, `r1/r2/r3_loader` | не в R4 | не компилируются |

Ввод: DirectInput открыт с `DISCL_FOREGROUND`, окно никогда не на переднем плане, `Acquire` не проходит. Захвата нет.

## 5. `g_dedicated_server` в xrGame и xrServerEntities

Правило. **Исключено** значит: на Mk идёт обычный путь одиночной игры (`!g_dedicated_server || g_coop_headless`). Обычный путь проверен
всей игрой, поэтому в сомнении место исключается. **Оставлено** значит: пропуск и на Mk, он безопасен и что-то экономит. Единственная
зависимость исключённых путей: UI должен существовать, поэтому он создан целиком, но не рисуется.

### Опасные, исключены (52 места xrGame и 2 xrServerEntities)

| Файл:строка | Что пропускалось | Почему опасно для процесса с ALife, Lua и призраком |
|---|---|---|
| `ai_space.cpp:46` | весь `init`: графы, укрытия, патрули, **Lua** | нет ни ИИ, ни скриптов |
| `ai_space.cpp:147` | `unload` | утечка уровня, Lua не выгружается |
| `ai_space.cpp:194, 205` | патрульные пути | `patrol()` падает |
| `GameObject.cpp:62, 98` | `CAI_ObjectLocation` | `ai_location()` разыменует NULL у каждого объекта |
| `GameObject.cpp:387, 391, 1114` | `CScriptBinder` | биндеры Lua не работают: мёртвая логика всех объектов и smart terrain |
| `Actor.cpp:232, 308, 321`, `Actor_Network.cpp:798` | память актёра | `m_memory` NULL при обращениях из логики |
| `Actor_Network.cpp:683, 726` | `client_spawn_manager` | держатель (транспорт) актёра |
| `Actor_Network.cpp:731` | `MapManager` | метки на удалённый объект |
| `Level.cpp:251` | `CMapManager`, `CGameTaskManager` | `Level().MapManager()` и задания из Lua на NULL |
| `Level.cpp:258` | ограничители, `client_spawn_manager`, автосейвы, звуковой менеджер | ограничители нужны каждому NPC |
| `Level.cpp:333` | снятие процесса Lua | |
| `Level.cpp:1046` | обновление карты и заданий | |
| `Level.cpp:1168` | **шаг GC Lua** | память Lua росла бы без ограничений |
| `Level_load.cpp:34` | патрули без ALife | на Mk ALife есть, для единообразия |
| `Level_load.cpp:173` | процесс скриптов уровня | |
| `Level_network.cpp:86, 92, 107, 185` | очистка ограничителей, GC, спавн-менеджера | всё это теперь существует |
| `Level_network_spawn.cpp:131, 149` | `client_spawn_manager` | обратные вызовы спавна |
| `Level_network_start_client.cpp:252` | `g_hud->Load`, `OnConnected` | `CurrentGameUI()` NULL, а его читают и C++, и Lua (`get_hud()`) |
| `Level_start.cpp:340` | `OnConnected` UI | то же |
| `game_sv_base.cpp:451, 670, 702` | **игровой процесс Lua** | `_g.start_game` и обновление скриптов |
| `game_base.cpp:40` | `GAME_PLAYER_FLAG_SKIP` вместо состояния игрока | сервер считал бы своего клиента пропущенным |
| `xrgame_dll_detach.cpp:51, 84` | диалоги, шейдеры инвентаря | диалоги из Lua |
| `CustomMonster.cpp:128` | `client_spawn_manager` | |
| `hit_memory_manager.cpp:389`, `visual_memory_manager.cpp:967`, `sound_memory_manager.cpp:526` | `client_spawn_manager` | отложенные объекты памяти NPC |
| `artefact_activation.cpp:189`, `Missile.cpp:199`, `RocketLauncher.cpp:36`, `Weapon.cpp:1667`, `WeaponStatMgunFire.cpp:809`, `inventory_item.cpp:309` | вершина AI у спавна: `u32(-1)` | в ALife объект с негодной вершиной |
| `MainMenu.cpp:101` | `g_btnHint`, `g_statHint`, окна ошибок | `CUIButton`, `CUIStatic` читают их без проверки |
| `ui_base.cpp:215` | `CUICursor`, `CFontManager` | без него нет шрифтов всего UI |
| `ui/UIWindow_script.cpp:107, 115` | шрифты из Lua | |
| `Spectator.cpp:543` | `MapManager` | наблюдатель: механизм призрака |
| `object_factory_register.cpp:439` | `_s`-классы вместо скриптов | двойная регистрация clsid, падение при старте Lua |
| `object_factory_script.cpp:75` | `ai()` при регистрации классов | |

### Оставлены

| Файл:строка | Что пропускается | Почему годится |
|---|---|---|
| `Actor.cpp:456` | загрузка звуков актёра | звука нет, блок только про звук (проверено чтением) |
| `Actor.cpp:612, 952, 1944` | звуки попадания, смерти, дыхания | то же |
| `Actor.cpp:648, 664` | индикаторы попадания HUD | рисовать некому |
| `GamePersistent.cpp:329` | окружающие звуки и частицы погоды | только звук, частицы, порыв ветра для глаз |
| `GamePersistent.cpp:545, 722, 724` | заставки, события интро | у Mk их нет |
| `Level.cpp:1060` | статистика на экран | рисовать негде, `CGameFont` копил бы строки |
| `Level.cpp:1156`, `Level_load.cpp:97` | статические звуки уровня, геометрия звука, случайные звуки | звука нет, экономит загрузку |
| `Level_bullet_manager_firetrace.cpp:190` | кровь на скелетах | рендер |
| `Level_network_start_client.cpp:222` | выгрузка текстур на карту | текстур нет |
| `Level_start.cpp:135, 169` | заголовок загрузки | экран загрузки не рисуется |
| `Level_start.cpp:281, 304, 325` | MP: загрузка карты, меню | путь MP. На Mk любой отказ запуска уровня теперь фатален: `Level_start.cpp:277` |
| `MainMenu.cpp:198` | открытие меню | **нужно**: меню на Mk не открывается |
| `ParticlesObject.cpp` (16 мест) | все частицы | **нужно**: визуала нет, заглушки покрывают весь интерфейс |
| `Weapon.cpp:405, 801`, `WeaponMagazined.cpp:1541` | окно прицела | при обращении проверяется на NULL, экономит окно на каждое оружие |
| `WeaponBinocularsVision.cpp:239` | метки бинокля | только для глаз |
| `xrServer.cpp:59` | `m_bDedicated` | читается только на `dwMaxPlayers` (`NET_Server.cpp:367`), `IsDedicated()` никто не вызывает |
| `game_sv_base.cpp:831` | MP-событие | на одиночной игре выход раньше, по `psNET_direct_connect` |

### Мёртвое на Mk

MP-типы игры, на одиночной игре объекты этих классов не создаются: `game_cl_artefacthunt.cpp`, `game_cl_capture_the_artefact.cpp`,
`game_cl_deathmatch.cpp`, `game_cl_mp.cpp`, `game_cl_teamdeathmatch.cpp`, `game_sv_capture_the_artefact.cpp`, `game_sv_deathmatch.cpp`,
`game_sv_mp.cpp`, `xrGameSpyServer.cpp`, `team_base_zone.cpp`, `xrServer.cpp:1243, 1261`, `Actor.cpp:581`.
Под `DEBUG`: `CustomMonster.cpp:125`, `Level_network.cpp:103`. В комментарии: `GameObject.cpp:1185`, `game_base.cpp:223`, `game_sv_base.cpp:312`.

## 6. Что ещё трогает весь рабочий стол (флагов там не было)

Процесс Mk живёт рядом с игрой игрока-хоста, поэтому отдельно закрыто всё глобальное:

| Что | Где | Опасность |
|---|---|---|
| `SetForegroundWindow`, `SetActiveWindow`, `SetWindowPos` без `NOACTIVATE` | `device.cpp`, `Device_create.cpp` | Mk забирал бы фокус у игры при старте |
| `ClipCursor` | `UpdateCursorClip`, `CRenderDevice::Destroy` | глобальный: поймал бы курсор игрока или снял клип игры при выходе Mk |
| `SetCursorPos` | `UICursor.cpp`, `imgui_base.cpp` | дёргал бы курсор игрока |
| Буфер обмена | `xrDebugNew.cpp` | падение Mk затирало бы буфер игрока |
| `SetGammaControl` | `xr_effgamma.cpp` | гамма монитора |
| `SystemParametersInfo` | `damn_keys_filter` | заставка, залипание клавиш |
| Steam | `coop_steam_overlay_start` | `RAYLINK_STEAM_API` наследуется от родителя |
| Discord, ReShade | `Startup` | вторая активность, второй аддон |
| Заставка | `WinMain_impl` | окно на экране |

Окно: `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, не показывается никогда. Нет в панели задач и в Alt-Tab.

## 7. Падение без окна

Режим `Debug.set_silent_fatal()` (`xrCore`, ключ включает его до всего остального):
- `xrDebug::backend`, `do_exit`, `UnhandledFilter`: без `MessageBox`, запись в лог, `TerminateProcess(..., 1)`;
- обработчики падения выполняются (сброс связи коопа, дамп переменных), стек пишется в лог, сейва при падении нет;
- сторож: через 30 с после начала падения процесс завершается с кодом 3, даже если обработчик завис на замке упавшего потока;
- `SEM_NOGPFAULTERRORBOX`, `SEM_FAILCRITICALERRORS`, `SEM_NOOPENFILEERRORBOX`: окна самой Windows (порча кучи, fast fail мимо фильтра,
  не найденная DLL) тоже закрыты. Режим наследуется дочерними процессами, у Mk их нет;
- отказ запуска уровня: `FATAL` в `Level_start.cpp`, иначе процесс остался бы ждать меню, которого нет.

Стоковые `xrDebug.cpp` и BugTrap в DX11-AVX не собираются (`ExcludedFromBuild`, `NO_BUG_TRAP`). `abort`, чистая виртуальная функция,
неверный параметр CRT, нехватка памяти идут через `backend`. Коды выхода: 1 падение, 3 сторож, 0 штатный выход.

## 8. Статистика

`coop_mapserver_stats` (`xrGame/coop/coop_mapserver_stats.cpp`): консольная команда на любом процессе, на Mk ещё и строка раз в 60 с:

```
[coop] coop_mapserver_stats: private 1832 MB, working set 1490 MB, lua 612 MB, alife 18211 objects, 214 online, frame avg 11.2 ms, worst 41 ms, 1797 frames
```

Кадр: работа без `Sleep`, бюджет 33 мс, счётчик сбрасывается при каждой строке. На процессе хоста в кадр входит рендер.

## 9. Риски

1. **NPC на Mk глухие.** `feel_sound_new` приходит только от живых источников звука (`IGame_Level::SoundEvent_Dispatch`, нужен `S->feedback`),
   с `-nosound` их нет. На Mk NPC реагируют на попадания и зрение, но не на выстрелы и шаги. Звук отключён по заданию. Вариант исправления: OpenAL Soft
   с `ALSOFT_DRIVERS=null` (устройство без вывода), источники живут, цена: кэш звука и декодирование ogg. Решение за ведущей.
2. **Яркость актёра.** `CROS_impl` считается в рендере, `object_luminocity` для игрока на Mk будет значением по умолчанию: заметность в темноте не учтена.
3. **Модели НЕ разгружены до конца.** Кости и CPU-копии остаются, `-noprefetch` в команду запуска экономит ещё, в коде не навязан.
4. **ЦП.** `UpdateCL` всех объектов каждый кадр дороже режима «ворон», замерить. `CalculateBonesThread` оставлен: он считает кости в 250 м от призрака.
5. **UI без рендера.** Весь UI создаётся из XML, `Draw` не вызывается, `Update` идёт. Это обычный код, но без рисования он не запускался.

## 10. Живой запуск (ведущая)

Ключ сам ещё ничего не грузит: уровень задаёт `-start`, он обязан быть последним. Аргументы ключа читает T9, призрак §1.6 тоже T9.

Подготовка, стенд `E:\AnomalyCoop` (GAMMA: `E:\AnomalyCoopGamma`, через MO2, виртуальная ФС наследуется только дочерним процессам):
1. `fsgame_ms1.ltx`: копия `fsgame_a.ltx`, в строке `$app_data_root$` вместо `appdata_a\` стоит `appdata_ms1\`;
2. `appdata_ms1\savedgames\`: сейв для проверки, `appdata_ms1\user.ltx`: копия из `appdata_a`.

```powershell
$p = Start-Process -FilePath .\bin\AnomalyCoopDX11AVX.exe -WorkingDirectory E:\AnomalyCoop -PassThru -ArgumentList @(
    '-coop_mapserver', 'l01_escape', '0', '27961', 'test', '-fsltx', 'fsgame_ms1.ltx', '-noprefetch',
    '-start', 'server(<сейв>/single/alife/load)', 'client(localhost)')
```

Смотреть в `appdata_ms1\logs\xray_<имя>.log`:

| Строка | Что значит |
|---|---|
| `* [coop] map server: WARP device, no swap chain, 64x64 targets` | устройство создано |
| нет `* GPU [vendor` , нет `[Discord RPC]`, нет `Steam overlay` | ключ сработал до старта |
| `[ReShade]: ReShade not installed` | ожидаемо, строка стоковая |
| `* [x-ray]: Starting Main Loop`, затем `* phase time` | дошло до цикла, уровень грузится |
| `[coop] coop_mapserver_stats:` раз в 60 с | `frames` около 1800, `avg` меньше 33 мс |
| `FATAL ERROR`, `stack trace`, `at address` | падение: первое, что смотреть |

Снаружи процесса:
- `(Get-Process -Id $p.Id).MainWindowHandle` равен 0, в панели задач и Alt-Tab пусто;
- при запуске и выходе Mk игра хоста не теряет фокус, курсор не дёргается и остаётся в окне игры, гамма монитора не меняется;
- `Get-Process -Id $p.Id | Select PM, WS` сходится со строкой статистики, загрузка видеокарты от процесса нулевая, в `Modules` нет `nvwgf2umx`, `nvapi64`, есть `d3d10warp`;
- падение: `server(nosuchsave/single/alife/load)`: окна нет, процесс завершился, `$p.ExitCode` не 0, в логе причина.

Порядок проверки, если упадёт на загрузке, идёт от самого вероятного:
1. код R4 с нулевыми шейдерами (`Resources->Create` возвращает NULL, этот режим на R4 никто не запускал);
2. UI без текстур и рисования;
3. цель 64×64 и UI-масштаб;
4. WARP не прошёл: `D3D11CreateDevice`, `CreateRenderTargetView`.

Что сравнить на GAMMA на картах Кордон, Затон, Юпитер: `private`, `lua`, `avg`, `worst`, план §1.8 обещает 1,5–2,5 ГБ.
