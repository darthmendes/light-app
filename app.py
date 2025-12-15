from flask import Flask, request, jsonify, render_template
import requests
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import time
from threading import Thread, Lock

app = Flask(__name__)

# Map station ID to IP
STATION_MAP = {
    "1" : "192.168.4.1", # default for AP 
    # Add more as needed
}

TIMEOUT = 2 

ambient_colors = {
    'left': [{'r': 0, 'g': 0, 'b': 0}] * 3, 
    'right': [{'r': 0, 'g': 0, 'b': 0}] * 3
}
ambient_lock = Lock()

def ambient_color_updater():
    """Connect to ambient server and update colors"""
    global ambient_colors
    while True:
        try:
            import websockets
            import asyncio
            
            async def update_colors():
                async with websockets.connect('ws://localhost:8765') as websocket:
                    print("Connected to ambient server")
                    while True:
                        message = await websocket.recv()
                        data = json.loads(message)
                        if data['type'] == 'ambient_colors':
                            with ambient_lock:
                                # Handle segmented format from new ambient.py
                                left_seg = data['colors'].get('left', [{'r':0,'g':0,'b':0}] * 3)
                                right_seg = data['colors'].get('right', [{'r':0,'g':0,'b':0}] * 3)
                                # Ensure we always have 3 segments (pad or truncate)
                                ambient_colors['left'] = (left_seg[:3] + [{'r':0,'g':0,'b':0}] * 3)[:3]
                                ambient_colors['right'] = (right_seg[:3] + [{'r':0,'g':0,'b':0}] * 3)[:3]
            
            asyncio.run(update_colors())
        except Exception as e:
            print(f"Ambient updater error: {e}")
            time.sleep(2)

Thread(target=ambient_color_updater, daemon=True).start()

@app.route("/api/ambient-colors")
def get_ambient_colors():
    with ambient_lock:
        return jsonify(ambient_colors)

def send_to_station(sid, ip, path, params=None):
    try:
        url = f"http://{ip}{path}"
        resp = requests.get(url, params=params, timeout=TIMEOUT)
        return {
            "station_id": sid,
            "ip": ip,
            "status": "success",
            "response": resp.text.strip()
        }
    except Exception as e:
        return {
            "station_id": sid,
            "ip": ip,
            "status": "error",
            "error": str(e)
        }

def send_to_stations(station_ids, path, params=None):
    futures = []
    results = []
    
    with ThreadPoolExecutor(max_workers=10) as executor:
        for sid in station_ids:
            ip = STATION_MAP.get(sid)
            if not ip:
                results.append({
                    "station_id": sid,
                    "status": "error",
                    "error": f"Unknown station ID: {sid}"
                })
                continue
            future = executor.submit(send_to_station, sid, ip, path, params)
            futures.append(future)
        
        for future in as_completed(futures):
            results.append(future.result())
    
    return results

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/trigger-all", methods=["POST"])
def trigger_all_lightning():
    results = send_to_stations(list(STATION_MAP.keys()), "/trigger")
    success = all(r["status"] == "success" for r in results)
    return jsonify({
        "status": "success" if success else "partial",
        "station_results": results
    })

@app.route("/style", methods=["POST"])
def send_style():
    data = request.get_json()
    if not data:
        return jsonify({"error": "No JSON payload"}), 400

    station_ids = data.get("stations")
    style = data.get("style")
    if not station_ids or not style:
        return jsonify({"error": "Missing 'stations' or 'style'"}), 400

    params = {}

    # Range
    params["start"] = max(0, min(59, int(style.get("start", 0))))
    params["end"] = max(0, min(59, int(style.get("end", 59))))

    # Base mode
    base = style.get("base", "off")
    params["name"] = base

    # Colors
    if "colors" in style:
        colors = style["colors"]
        params["colors"] = len(colors)
        for i, col in enumerate(colors):
            params[f"r{i}"] = max(0, min(255, int(col.get("r", 0))))
            params[f"g{i}"] = max(0, min(255, int(col.get("g", 0))))
            params[f"b{i}"] = max(0, min(255, int(col.get("b", 0))))

    # Thunderstorm flash
    if base == "thunderstorm" and "flash" in style:
        flash = style["flash"]
        params["c2r"] = max(0, min(255, int(flash.get("r", 255))))
        params["c2g"] = max(0, min(255, int(flash.get("g", 255))))
        params["c2b"] = max(0, min(255, int(flash.get("b", 255))))

    # Overlay
    if "overlay" in style:
        params["overlay"] = style["overlay"]
        overlay_color = style.get("overlayColor", {"r":255,"g":255,"b":255})
        params["or"] = max(0, min(255, int(overlay_color.get("r", 255))))
        params["og"] = max(0, min(255, int(overlay_color.get("g", 255))))
        params["ob"] = max(0, min(255, int(overlay_color.get("b", 255))))

    # Advanced params
    for key in ["speed", "intensity", "brightness", "size"]:
        if key in style:
            val = int(style[key])
            if key in ["intensity", "brightness"]:
                params[key] = max(0, min(255, val))
            elif key == "speed":
                params[key] = max(1, min(10, val))
            elif key == "size":
                params[key] = max(1, min(20, val))

    if "direction" in style:
        params["dir"] = "b" if style["direction"] == "backward" else "f"

    #  Handle segmented ambient (forward all seg_* params)
    if "ambient_segments" in style:
        params["ambient_segments"] = style["ambient_segments"]
        params["ambient_blend"] = max(0, min(255, int(style.get("ambient_blend", 128))))
        # Forward ALL seg_* parameters directly
        for key, value in style.items():
            if key.startswith("seg_"):
                params[key] = max(0, min(255, int(value)))
    #  Also keep legacy ambient support (for backward compatibility)
    elif "ambient" in style:
        ambient = style["ambient"]
        params["ambient_r"] = max(0, min(255, int(ambient.get("r", 0))))
        params["ambient_g"] = max(0, min(255, int(ambient.get("g", 0))))
        params["ambient_b"] = max(0, min(255, int(ambient.get("b", 0))))
        params["ambient_blend"] = max(0, min(255, int(ambient.get("blend", 128))))
    print(params)
    results = send_to_stations(station_ids, "/mode", params)
    success = all(r["status"] == "success" for r in results)
    return jsonify({
        "status": "success" if success else "partial",
        "station_results": results
    })

if __name__ == "__main__":
    print(f"D&D LED Controller started!")
    print(f"Stations: {STATION_MAP}")
    print(f"Open http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)