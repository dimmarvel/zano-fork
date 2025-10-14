# pip install requests
import json
import requests
from typing import List, Tuple

# RPC endpoints
BLOCKS_RPC_URL = "http://127.0.0.1:11211/json_rpc"      # get_blocks_details
TX_RPC_URL     = "http://37.27.100.59:10500/json_rpc"   # get_tx_details
HEADERS = {"Content-Type": "application/json"}

# Конфиг
BLOCKS_PER_HEIGHT = 10     # H..H+9
TXS_PER_HEIGHT    = 10     # сколько tx печатать для каждой высоты
START_HEIGHTS = [
    190_397,
    256_000,
    1_055_580,
    2_111_111,
    3_000_000,
    3_373_378,
]

def rpc_call(url: str, method: str, params: dict, req_id: int = 0, timeout: int = 30) -> dict:
    payload = {"jsonrpc": "2.0", "method": method, "params": params, "id": req_id}
    r = requests.post(url, headers=HEADERS, json=payload, timeout=timeout)
    r.raise_for_status()
    data = r.json()
    if "error" in data:
        raise RuntimeError(f"RPC error: {data['error']}")
    return data

def get_blocks_details(height_start: int, count: int) -> List[dict]:
    data = rpc_call(
        BLOCKS_RPC_URL,
        "get_blocks_details",
        {"count": count, "height_start": height_start, "ignore_transactions": False},
        req_id=0,
    )
    result = data.get("result", {})
    if result.get("status") != "OK":
        raise RuntimeError(f"Bad status from get_blocks_details: {result.get('status')}")
    return result.get("blocks", []) or []

def extract_tx_ids_with_heights(blocks: List[dict]) -> List[Tuple[str, int]]:
    """
    Возвращает (txid, height). Сначала из transactions_details, затем, если есть, из object_in_json.tx_hashes.
    Порядок сохранён, дубликаты удалены.
    """
    seen = set()
    ordered: List[Tuple[str, int]] = []

    for b in blocks:
        h = b.get("height")
        # из transactions_details
        for tx in b.get("transactions_details", []) or []:
            txid = tx.get("id")
            if isinstance(txid, str) and txid not in seen:
                seen.add(txid)
                keeper_h = tx.get("keeper_block", h)
                ordered.append((txid, keeper_h))
        # иногда tx_hashes в object_in_json
        obj = b.get("object_in_json")
        parsed = None
        if isinstance(obj, str) and obj.strip().startswith("{"):
            try:
                parsed = json.loads(obj)
            except Exception:
                parsed = None
        elif isinstance(obj, dict):
            parsed = obj
        if isinstance(parsed, dict) and isinstance(parsed.get("tx_hashes"), list):
            for txid in parsed["tx_hashes"]:
                if isinstance(txid, str) and txid not in seen:
                    seen.add(txid)
                    ordered.append((txid, h))
    return ordered

def get_tx_blob(tx_hash: str) -> str:
    data = rpc_call(TX_RPC_URL, "get_tx_details", {"tx_hash": tx_hash}, req_id=1)
    result = data.get("result", {})
    if result.get("status") != "OK":
        return ""
    return (result.get("tx_info") or {}).get("blob", "") or ""

def pretty_height(n: int) -> str:
    # 1234567 -> "1 234 567"
    return f"{n:,}".replace(",", " ")

def raw_string_literal(s: str) -> str:
    """
    Возвращает безопасный C++ raw string literal для произвольной строки s.
    Подбираем делимитер, чтобы внутри не встретилось )DELIM".
    """
    for delim in ["", "TX", "END", "BLOB", "XYZ", "DELIM", "RAW", "SEP___", "ZZ"]:
        endseq = f"){delim}\""
        if endseq not in s:
            return f'R"{delim}({s}){delim}"'
    # Фолбэк: обычная строка с экранированием (на всякий случай)
    esc = s.replace("\\", "\\\\").replace('"', '\\"')
    return f"\"{esc}\""

def collect_entries() -> List[Tuple[int, str, str]]:
    """
    Собирает [(height, tx_hash, blob)] для всех заданных стартовых высот.
    На каждую высоту — первые 10 уникальных транзакций из H..H+9.
    """
    entries: List[Tuple[int, str, str]] = []
    for H in START_HEIGHTS:
        blocks = get_blocks_details(H, BLOCKS_PER_HEIGHT)
        txs = extract_tx_ids_with_heights(blocks)[:TXS_PER_HEIGHT]
        for txid, h in txs:
            blob = get_tx_blob(txid)
            entries.append((h, txid, blob))
    return entries

def emit_cpp_vector(entries: List[Tuple[int, str, str]]) -> str:
    """
    Формирует C++ код:
      struct tx_data { std::string tx_blob; std::string tx_hash; };
      std::vector<tx_data> real_txs = { ... };

    Для каждой записи добавляется комментарий:
      // <height> height
    """
    lines = []
    lines.append('#include <string>')
    lines.append('#include <vector>')
    lines.append('')
    lines.append('struct tx_data {')
    lines.append('  std::string tx_blob;')
    lines.append('  std::string tx_hash;')
    lines.append('};')
    lines.append('')
    lines.append('std::vector<tx_data> real_txs = {')
    for h, txid, blob in entries:
        lines.append(f'  // {pretty_height(h)} height')
        blob_lit = raw_string_literal(blob) if blob else 'R""'
        # tx_hash — обычная строка (hex), экранирование не требуется
        lines.append(f'  {{{blob_lit}, "{txid}"}},')
    lines.append('};')
    return "\n".join(lines)

def main():
    entries = collect_entries()
    cpp = emit_cpp_vector(entries)
    print(cpp)

if __name__ == "__main__":
    main()
