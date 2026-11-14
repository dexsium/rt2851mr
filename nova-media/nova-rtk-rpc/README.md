# nova-rtk-rpc

Userspace-реализация Realtek Video RPC протокола (ARM↔AVCPU) для Nova OS.

| Файл | Назначение | Статус |
|---|---|---|
| `nova_rpc.h/.c` | Низкоуровневый транспорт: `/dev/rpc100`, register/stop/read | IMPLEMENTED, COMPILED |
| `nova_rpc_protocol.h` | Таблица подтверждённых числовых ID RPC-команд (с точными ссылками на файл:строку оригинала) | CONFIRMED |
| `nova_video_rpc.h/.c` | DEC-команды (Create/Init/Connect/Run/Pause/Stop/Flush/Destroy) + структуры FrameInfo/PicInfo/MediaInfo | IMPLEMENTED (структуры), NOT IMPLEMENTED (отправка) |
| `nova_vo_rpc.h/.c` | VO FILTER-команды (ConnectVDec/Display/ClearVideo/SetSpeed) | IMPLEMENTED (структуры), NOT IMPLEMENTED (отправка) |

**Почему "отправка" не реализована:** `rpcapi.h` (открытый userspace-заголовок) даёт только
`registerRPC()`/`stopRPC()`/`readRingBuf()` — общий транспорт. Он не документирует формат
заголовка одного RPC-сообщения (как упаковать конкретный `procedureID` + аргументы в то, что
реально читает AVCPU). Кернел-функция `send_rpc_command()` (`rtk_vdec_driver.c`) делает это на
стороне ядра, но её внутренняя реализация (как именно она пишет в кольцевой буфер) не была
прочитана построчно в этой сессии. Поэтому каждая публичная функция здесь честно вызывает
`nova_rpc_call_TODO()`/`nova_vo_rpc_call_TODO()` — единственное место, которое нужно дописать,
когда формат сообщения будет восстановлен (см. `NOVA_REALTEK_RPC_VO_PROTOCOL_AUDIT.md`, раздел
"следующие шаги"). Это не заглушка-обман — она возвращает `-ENOSYS`, а не притворяется успехом.

Все числовые идентификаторы и структуры — не придуманы, скопированы буквально из открытых
сгенерированных заголовков `kernel/drivers/rtk_kdriver/common/IPC/generate/include/video/`.
