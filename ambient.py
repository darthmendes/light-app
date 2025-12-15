# ambient.py
import asyncio
import websockets
import json
import numpy as np
from mss import mss
import cv2
import tkinter as tk

# ✅ CONFIGURE NUMBER OF SEGMENTS PER SIDE
SEGMENTS_PER_SIDE = 3  # You can try 5 for smoother gradients (60 LEDs → 12 per segment)

def get_screen_regions():
    """Auto-detect screen size and create safe regions"""
    try:
        root = tk.Tk()
        root.withdraw()
        width = root.winfo_screenwidth()
        height = root.winfo_screenheight()
        root.destroy()
        
        margin_w = min(200, width // 10)
        margin_h = min(200, height // 10)
        
        return {
            'left': {
                'top': margin_h,
                'left': 0,
                'width': margin_w,
                'height': height - 2 * margin_h
            },
            'right': {
                'top': margin_h,
                'left': width - margin_w,
                'width': margin_w,
                'height': height - 2 * margin_h
            },
            'top': {
                'top': 0,
                'left': 0,
                'width': width,
                'height': margin_h
            },
            'bottom': {
                'top': height - margin_h,
                'left': 0,
                'width': width,
                'height': margin_h
            }
        }
    except Exception as e:
        print(f"Screen detection failed: {e}")
        return {
            'left': {'top': 100, 'left': 0, 'width': 150, 'height': 880},
            'right': {'top': 100, 'left': 1770, 'width': 150, 'height': 880},
            'top': {'top': 0, 'left': 0, 'width': 1920, 'height': 100},
            'bottom': {'top': 980, 'left': 0, 'width': 1920, 'height': 100}
        }

REGIONS = get_screen_regions()
print(f"Screen regions: {REGIONS}")
print(f"Using {SEGMENTS_PER_SIDE} segments per side")

connected_clients = set()

async def screen_capture():
    """Capture screen and send segmented color data"""
    with mss() as sct:
        monitor = sct.monitors[0]
        screen_w = monitor['width']
        screen_h = monitor['height']
        print(f"Screen size: {screen_w}x{screen_h}")
        
        frame_count = 0
        while True:
            try:
                all_colors = {}
                all_black = True
                
                for name, region in REGIONS.items():
                    segment_colors = []
                    
                    # Vertical segmentation for left/right
                    if name in ['left', 'right']:
                        seg_height = max(1, region['height'] // SEGMENTS_PER_SIDE)
                        for i in range(SEGMENTS_PER_SIDE):
                            seg_top = region['top'] + i * seg_height
                            # Last segment takes remaining pixels
                            seg_h = region['height'] - i * seg_height if i == SEGMENTS_PER_SIDE - 1 else seg_height
                            
                            safe_region = {
                                'top': max(0, min(seg_top, screen_h - 1)),
                                'left': max(0, min(region['left'], screen_w - 1)),
                                'width': max(1, min(region['width'], screen_w - region['left'])),
                                'height': max(1, min(seg_h, screen_h - seg_top))
                            }
                            
                            try:
                                screenshot = sct.grab(safe_region)
                                frame = np.array(screenshot)
                                if frame.size > 0:
                                    avg = cv2.mean(frame)[:3]
                                    r, g, b = int(avg[2]), int(avg[1]), int(avg[0])
                                    segment_colors.append({'r': r, 'g': g, 'b': b})
                                    if r > 10 or g > 10 or b > 10:
                                        all_black = False
                                else:
                                    segment_colors.append({'r': 0, 'g': 0, 'b': 0})
                            except Exception as e:
                                print(f"Capture error for {name} segment {i}: {e}")
                                segment_colors.append({'r': 0, 'g': 0, 'b': 0})
                    
                    # Horizontal segmentation for top/bottom (future-proofing)
                    elif name in ['top', 'bottom']:
                        seg_width = max(1, region['width'] // SEGMENTS_PER_SIDE)
                        for i in range(SEGMENTS_PER_SIDE):
                            seg_left = region['left'] + i * seg_width
                            seg_w = region['width'] - i * seg_width if i == SEGMENTS_PER_SIDE - 1 else seg_width
                            
                            safe_region = {
                                'top': max(0, min(region['top'], screen_h - 1)),
                                'left': max(0, min(seg_left, screen_w - 1)),
                                'width': max(1, min(seg_w, screen_w - seg_left)),
                                'height': max(1, min(region['height'], screen_h - region['top']))
                            }
                            
                            try:
                                screenshot = sct.grab(safe_region)
                                frame = np.array(screenshot)
                                if frame.size > 0:
                                    avg = cv2.mean(frame)[:3]
                                    r, g, b = int(avg[2]), int(avg[1]), int(avg[0])
                                    segment_colors.append({'r': r, 'g': g, 'b': b})
                                    if r > 10 or g > 10 or b > 10:
                                        all_black = False
                                else:
                                    segment_colors.append({'r': 0, 'g': 0, 'b': 0})
                            except Exception as e:
                                print(f"Capture error for {name} segment {i}: {e}")
                                segment_colors.append({'r': 0, 'g': 0, 'b': 0})
                    
                    all_colors[name] = segment_colors

                # Debug print every 30 frames
                if frame_count % 30 == 0:
                    print(f"Segmented colors: {all_colors}")
                    if all_black:
                        print("⚠️ All segments are black! Check screen content.")
                
                frame_count += 1

                # Broadcast to all clients
                if connected_clients:
                    message = json.dumps({'type': 'ambient_colors', 'colors': all_colors})
                    disconnected = set()
                    for client in connected_clients:
                        try:
                            await client.send(message)
                        except Exception as e:
                            print(f"Client send error: {e}")
                            disconnected.add(client)
                    connected_clients.difference_update(disconnected)
                
                # Target ~30 FPS
                await asyncio.sleep(0.033)
                
            except Exception as e:
                print(f"Main capture error: {e}")
                await asyncio.sleep(1)

async def handle_client(websocket):
    print("Client connected")
    connected_clients.add(websocket)
    try:
        async for message in websocket:
            try:
                data = json.loads(message)
                if data.get('type') == 'ping':
                    await websocket.send(json.dumps({'type': 'pong'}))
            except Exception as e:
                print(f"Message error: {e}")
    except Exception as e:
        print(f"Client error: {e}")
    finally:
        connected_clients.discard(websocket)
        print("Client disconnected")

async def main():
    print("Starting segmented ambient screen capture...")
    capture_task = asyncio.create_task(screen_capture())
    print(f"Ambient server starting on ws://localhost:8765 (segments={SEGMENTS_PER_SIDE})")
    async with websockets.serve(handle_client, "localhost", 8765):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())