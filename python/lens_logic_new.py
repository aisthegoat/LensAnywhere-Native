import http.server
import socketserver
import threading
import subprocess
import re
import urllib.parse
import webbrowser
import os
import sys
import time

PORT = 8998  # Internal local server port
CURRENT_IMAGE = None
PUBLIC_URL = None
SERVER_READY = False

def get_resource_path(relative_path):
    """Get absolute path to resource, works for dev and for PyInstaller bundling."""
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, relative_path)
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), relative_path)


class ImageHandler(http.server.BaseHTTPRequestHandler):
    """Serves the latest captured screenshot from RAM."""
    def do_GET(self):
        global CURRENT_IMAGE
        if self.path.startswith("/image.png"):
            if CURRENT_IMAGE:
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(CURRENT_IMAGE)))
                self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
                self.end_headers()
                self.wfile.write(CURRENT_IMAGE)
            else:
                self.send_error(404, "No image captured yet")
        else:
            self.send_error(404, "Not Found")

    def log_message(self, format, *args):
        # Silence local HTTP server logs in console
        pass


def start_local_server():
    """Runs a tiny local web server in a background thread."""
    socketserver.TCPServer.allow_reuse_address = True
    try:
        with socketserver.TCPServer(("127.0.0.1", PORT), ImageHandler) as httpd:
            httpd.serve_forever()
    except Exception as e:
        print(f"Local server error: {e}")


def start_cloudflare_tunnel():
    """Launches cloudflared.exe and extracts the public HTTPS URL."""
    global PUBLIC_URL, SERVER_READY

    exe_name = "cloudflared.exe" if sys.platform == "win32" else "cloudflared"
    exe_path = get_resource_path(exe_name)

    if not os.path.exists(exe_path):
        print(f"\n[ERROR] '{exe_name}' not found in project directory!")
        print("Please download cloudflared.exe and place it in the same folder as this script.\n")
        return

    print("Initializing Cloudflare Tunnel in background...")

    creation_flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

    process = subprocess.Popen(
        [exe_path, "tunnel", "--url", f"http://127.0.0.1:{PORT}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        creationflags=creation_flags
    )

    url_regex = re.compile(r'https://[a-zA-Z0-9-]+\.trycloudflare\.com')

    for line in iter(process.stdout.readline, ''):
        match = url_regex.search(line)
        if match:
            PUBLIC_URL = match.group(0)
            SERVER_READY = True
            print(f"✓ Cloudflare Tunnel Active: {PUBLIC_URL}")
            break


def start_backend():
    """Initializes local server and tunnel on app launch."""
    t1 = threading.Thread(target=start_local_server, daemon=True)
    t1.start()

    t2 = threading.Thread(target=start_cloudflare_tunnel, daemon=True)
    t2.start()


# Automatically start local server & tunnel when app boots up
start_backend()


def is_server_ready():
    """Returns True if local server and Cloudflare tunnel are active and ready."""
    global SERVER_READY
    return SERVER_READY


def search_lens(image_bytes: bytes):
    """Serves the capture locally and opens Google Lens via the Cloudflare URL."""
    global CURRENT_IMAGE, PUBLIC_URL, SERVER_READY

    CURRENT_IMAGE = image_bytes

    # Wait briefly if the user captures immediately upon app launch
    timeout = 10
    start_time = time.time()
    while not SERVER_READY and (time.time() - start_time) < timeout:
        time.sleep(0.1)

    if not PUBLIC_URL:
        print("[ERROR] Tunnel unavailable. Make sure cloudflared.exe is in the project folder.")
        return

    # Add timestamp parameter to prevent browser image caching
    img_url = f"{PUBLIC_URL}/image.png?t={int(time.time() * 1000)}"
    lens_url = "https://lens.google.com/uploadbyurl?url=" + urllib.parse.quote(img_url, safe="")

    print(f"Opening Google Lens for captured region...")
    webbrowser.open(lens_url)


if __name__ == "__main__":
    print("This module is meant to be imported and run via main.py")