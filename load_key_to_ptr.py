import asyncio
import aiohttp
import random
import signal
from pathlib import Path
from aiohttp import ClientError, ClientConnectorError, ServerDisconnectedError, ClientOSError

URLS = [
    "http://127.0.0.1:11211/getblocks.bin",
    "http://127.0.0.1:11211/get_tx_pool.bin",
]

PARALLEL_CLIENTS_PER_URL = 10

# настройки ретраев
INITIAL_BACKOFF = 0.2      # стартовая задержка
MAX_BACKOFF = 10.0         # верхняя граница задержки между попытками
TIMEOUT_TOTAL = 30         # общий таймаут запроса (сек)

# читаем бинарники один раз
BIN_DATA = {
    "blocks": Path("getblocks.bin").read_bytes(),
    "pool":   Path("get_tx_pool.bin").read_bytes(),
}


async def spam(session: aiohttp.ClientSession, url: str, data: bytes, name: str):
    """
    Бесконечно шлёт POST-запросы на url.
    При сетевых ошибках — ретраит с экспоненциальным бэкоффом.
    """
    backoff = 0.0
    while True:
        try:
            # можно добавить timeout, чтобы зависшие коннекты корректно отваливались
            async with session.post(url, data=data, timeout=aiohttp.ClientTimeout(total=TIMEOUT_TOTAL)) as resp:
                # опционально проверять код ответа:
                # resp.raise_for_status()
                await resp.read()

            # если успешно — сбрасываем backoff
            backoff = 0.0

        except asyncio.CancelledError:
            # корректно выходим при отмене задачи
            raise

        except (ServerDisconnectedError,
                ClientConnectorError,
                ClientOSError,
                aiohttp.ClientPayloadError,
                aiohttp.ClientResponseError,
                asyncio.TimeoutError,
                ClientError) as e:
            # экспоненциальный бэкофф с небольшим джиттером
            backoff = INITIAL_BACKOFF if backoff == 0.0 else min(MAX_BACKOFF, backoff * 2)
            jitter = random.uniform(0, backoff * 0.2)
            delay = backoff + jitter
            # можно залогировать, если нужно
            # print(f"[{name}] {type(e).__name__}: {e}. retry in {delay:.2f}s")
            await asyncio.sleep(delay)
            # и просто пробуем снова — session сам переоткроет соединение при следующем .post()


async def main():
    # Коннектор с keepalive и ограничениями; limit=0 — не ограничивать общее число соединений,
    # но оставим разумные пределы
    connector = aiohttp.TCPConnector(limit=0, limit_per_host=0, ttl_dns_cache=60)

    timeout = aiohttp.ClientTimeout(total=None, connect=10)
    async with aiohttp.ClientSession(connector=connector, timeout=timeout) as session:
        tasks = []

        for i in range(PARALLEL_CLIENTS_PER_URL):
            data_blocks = BIN_DATA["blocks"]
            data_pool = BIN_DATA["pool"]

            t1 = asyncio.create_task(
                spam(session, URLS[0], data_blocks, name=f"blocks-{i+1}")
            )
            t2 = asyncio.create_task(
                spam(session, URLS[1], data_pool, name=f"pool-{i+1}")
            )
            tasks.extend([t1, t2])

        # аккуратная остановка по Ctrl+C/сигналу
        stop_event = asyncio.Event()

        def _graceful_shutdown():
            stop_event.set()

        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _graceful_shutdown)
            except NotImplementedError:
                # Windows может не поддерживать add_signal_handler для SIGTERM
                pass

        # ждём либо стоп-сигнал, либо вечности :)
        await stop_event.wait()

        # отменяем все таски и ждём их завершения
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
