import requests
import threading
import time

def rpc_call(i):
    try:
        url = "http://127.0.0.1:12111/json_rpc"
        data = {
            "jsonrpc": "2.0",
            "id": i,
            "method": "getblockheaderbyheight",
            "params": {"height": 1}
        }
        r = requests.post(url, json=data, timeout=10)
        print(f"Thread {i}: {r.status_code} {r.text}")
        time.sleep(2)  # delay for demonstration purposes
    except Exception as e:
        print(f"Thread {i} error: {e}")

threads = []
for i in range(900):  # 900 > 126
    t = threading.Thread(target=rpc_call, args=(i,))
    t.start()
    threads.append(t)

for t in threads:
    t.join()