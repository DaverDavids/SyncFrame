import configparser
import io
import hashlib
import json
import logging
import os
import queue
import secrets
import shutil
import ssl
import struct
import subprocess
import threading
import time
import zlib
from datetime import datetime, timedelta, timezone
from functools import wraps
from io import BytesIO

import paho.mqtt.client as mqtt
import pillow_heif
import schedule
from flask import (
    Blueprint,
    Flask,
    Response,
    redirect,
    render_template_string,
    request,
    send_file,
    send_from_directory,
    session,
    url_for,
    jsonify,
    stream_with_context,
)
from flask.sessions import SecureCookieSessionInterface
from itsdangerous import URLSafeTimedSerializer, BadSignature
from PIL import Image, ImageEnhance, ImageSequence
from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    CRYPTOGRAPHY_AVAILABLE = True
except Exception:
    CRYPTOGRAPHY_AVAILABLE = False

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(message)s")

DATA_DIR = os.path.join(os.getcwd(), "data")
os.makedirs(DATA_DIR, exist_ok=True)
MOSQ_DIR = os.path.join(DATA_DIR, "mosq")
os.makedirs(MOSQ_DIR, exist_ok=True)
OTA_DIR = os.path.join(DATA_DIR, "ota")
os.makedirs(OTA_DIR, exist_ok=True)

try:
    import pwd as _pwd
    _mosq = _pwd.getpwnam("mosquitto")
    os.chown(MOSQ_DIR, _mosq.pw_uid, _mosq.pw_gid)
except Exception:
    pass

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WATCH_FILE = os.path.abspath(os.path.join(DATA_DIR, "photo.jpg"))
PHOTO_ETAG_PATH = os.path.join(DATA_DIR, "photo_upload_etag.txt")
current_photo_hash = "00000000"
connected_stream_clients = {}
connected_stream_clients_lock = threading.Lock()


def _compute_photo_hash(path):
    try:
        with open(path, "rb") as f:
            return f"{zlib.crc32(f.read()) & 0xFFFFFFFF:08x}"
    except Exception:
        return "00000000"


def _build_mjpeg_part(jpeg_bytes):
    return (
        b"--frame\r\n"
        b"Content-Type: image/jpeg\r\n"
        + f"Content-Length: {len(jpeg_bytes)}\r\n\r\n".encode("ascii")
        + jpeg_bytes
        + b"\r\n"
    )


def _push_stream_frame(jpeg_bytes):
    part = _build_mjpeg_part(jpeg_bytes)
    with connected_stream_clients_lock:
        dead = []
        for client_id, q in connected_stream_clients.items():
            try:
                while q.qsize() > 1:
                    q.get_nowait()
                q.put_nowait(part)
            except Exception:
                dead.append(client_id)
        for client_id in dead:
            connected_stream_clients.pop(client_id, None)


for _fname in os.listdir(SCRIPT_DIR):
    if _fname.lower().endswith(".jpg"):
        _src = os.path.join(SCRIPT_DIR, _fname)
        _dst = os.path.join(DATA_DIR, _fname)
        if not os.path.exists(_dst):
            shutil.copy2(_src, _dst)

if os.path.exists(WATCH_FILE):
    current_photo_hash = _compute_photo_hash(WATCH_FILE)

CONFIG_PATH = os.path.join(DATA_DIR, "syncframe-server.conf")
config = configparser.ConfigParser()
default_config = {
    "server": {
        "host": "0.0.0.0",
        "port": "9369",
        "use_https": "true",
        "certfile": "./data/cert.pem",
        "keyfile": "./data/key.pem",
        "url_prefix": "/syncframe",
        "username": "admin",
        "password": "changeme",
        "secret_key": "",
    },
    "mqtt": {
        "host": "127.0.0.1",
        "port": "9368",
        "topic": "photos",
        "username": "mqttuser",
        "password": "mqttpass",
    },
    "watcher": {
        "file_to_watch": "./data/photo.jpg",
    },
    "image": {
        "max_width": "1920",
        "max_height": "1080",
        "jpeg_quality": "85",
    },
}
if not os.path.exists(CONFIG_PATH):
    config.read_dict(default_config)
    with open(CONFIG_PATH, "w") as cfgfile:
        config.write(cfgfile)
else:
    config.read(CONFIG_PATH)
    updated = False
    for section, values in default_config.items():
        if not config.has_section(section):
            config.add_section(section)
            updated = True
        for key, val in values.items():
            if not config.has_option(section, key):
                config.set(section, key, val)
                updated = True
    if updated:
        with open(CONFIG_PATH, "w") as cfgfile:
            config.write(cfgfile)

OLD_SECRET_KEY = "syncframe-secret-key-change-me-in-production"
secret_key_value = config.get("server", "secret_key", fallback="").strip()
if not secret_key_value:
    secret_key_value = secrets.token_hex(32)
    config.set("server", "secret_key", secret_key_value)
    with open(CONFIG_PATH, "w") as cfgfile:
        config.write(cfgfile)

SERVER_HOST = config.get("server", "host", fallback=default_config["server"]["host"])
SERVER_PORT = config.getint("server", "port", fallback=int(default_config["server"]["port"]))
USE_HTTPS = config.getboolean("server", "use_https", fallback=True)
CERTFILE = os.path.abspath(config.get("server", "certfile", fallback=default_config["server"]["certfile"]))
KEYFILE = os.path.abspath(config.get("server", "keyfile", fallback=default_config["server"]["keyfile"]))
_raw_prefix = config.get("server", "url_prefix", fallback=default_config["server"]["url_prefix"]).strip()
URL_PREFIX = "" if _raw_prefix in ("", "/") else "/" + _raw_prefix.strip("/")
USERNAME = config.get("server", "username", fallback=default_config["server"]["username"])
PASSWORD = config.get("server", "password", fallback=default_config["server"]["password"])
MQTT_HOST = config.get("mqtt", "host", fallback=default_config["mqtt"]["host"])
MQTT_PORT = config.getint("mqtt", "port", fallback=int(default_config["mqtt"]["port"]))
MQTT_TOPIC = config.get("mqtt", "topic", fallback=default_config["mqtt"]["topic"])
MQTT_USERNAME = config.get("mqtt", "username", fallback="")
MQTT_PASSWORD = config.get("mqtt", "password", fallback="")
WATCH_FILE = os.path.abspath(config.get("watcher", "file_to_watch", fallback=default_config["watcher"]["file_to_watch"]))
IMAGE_MAX_WIDTH = config.getint("image", "max_width", fallback=1920)
IMAGE_MAX_HEIGHT = config.getint("image", "max_height", fallback=1080)
IMAGE_JPEG_QUALITY = config.getint("image", "jpeg_quality", fallback=85)


def _load_photo_etag():
    if os.path.exists(PHOTO_ETAG_PATH):
        try:
            with open(PHOTO_ETAG_PATH, "r") as f:
                return f.read().strip()
        except Exception:
            pass
    return None


def _save_photo_etag(etag):
    try:
        with open(PHOTO_ETAG_PATH, "w") as f:
            f.write(etag)
    except Exception:
        pass


photo_upload_etag = _load_photo_etag() or secrets.token_hex(8)
_save_photo_etag(photo_upload_etag)

_sse_subscribers = []
_sse_lock = threading.Lock()


def _sse_notify(message: str = "refresh"):
    with _sse_lock:
        dead = []
        for q in _sse_subscribers:
            try:
                q.put_nowait(message)
            except Exception:
                dead.append(q)
        for q in dead:
            _sse_subscribers.remove(q)


app = Flask(__name__)
app.secret_key = secret_key_value


def check_auth(username, password):
    return username == USERNAME and password == PASSWORD


def authenticate():
    return Response("Authentication required", 401, {"WWW-Authenticate": 'Basic realm="Login Required"'})


def requires_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        auth = request.authorization
        if not auth or not check_auth(auth.username, auth.password):
            return authenticate()
        return f(*args, **kwargs)
    return decorated


@app.route(f"{URL_PREFIX}/events")
@requires_auth
def events():
    def gen():
        q = queue.SimpleQueue()
        with _sse_lock:
            _sse_subscribers.append(q)
        try:
            yield "retry: 5000\n\n"
            while True:
                try:
                    msg = q.get(timeout=15)
                    yield f"data: {msg}\n\n"
                except Exception:
                    yield ": keepalive\n\n"
        finally:
            with _sse_lock:
                if q in _sse_subscribers:
                    _sse_subscribers.remove(q)
    return Response(stream_with_context(gen()), mimetype="text/event-stream")


@app.route(f"{URL_PREFIX}/stream")
@requires_auth
def stream():
    client_id = secrets.token_hex(8)
    q = queue.Queue(maxsize=2)
    with connected_stream_clients_lock:
        connected_stream_clients[client_id] = q

    def gen():
        global current_photo_hash
        try:
            if os.path.exists(WATCH_FILE):
                with open(WATCH_FILE, "rb") as f:
                    yield _build_mjpeg_part(f.read())
            while True:
                try:
                    part = q.get(timeout=15)
                    yield part
                except queue.Empty:
                    yield b"--frame\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n"
        finally:
            with connected_stream_clients_lock:
                connected_stream_clients.pop(client_id, None)

    return Response(stream_with_context(gen()), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route(f"{URL_PREFIX}/photo.jpg")
@requires_auth
def photo():
    return send_file(WATCH_FILE, mimetype="image/jpeg")


@app.route(f"{URL_PREFIX}/api/status")
@requires_auth
def api_status():
    return jsonify({
        "ok": True,
        "mqtt": True,
        "http": True,
        "mjpeg": True,
        "lastMjpegConnectMs": int(time.time() * 1000),
        "photoHash": current_photo_hash,
    })


def generate_thumbnails(src_path):
    global current_photo_hash
    with Image.open(src_path) as img:
        rgb = img.convert("RGB")
        rgb.thumbnail((IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT))
        rgb.save(WATCH_FILE, format="JPEG", quality=IMAGE_JPEG_QUALITY, optimize=True)
    current_photo_hash = _compute_photo_hash(WATCH_FILE)
    with open(WATCH_FILE, "rb") as f:
        jpg = f.read()
    _push_stream_frame(jpg)
    _sse_notify("refresh")
    return True


@app.route(f"{URL_PREFIX}/upload", methods=["POST"])
@requires_auth
def upload():
    if "file" not in request.files:
        return jsonify({"ok": False, "error": "missing file"}), 400
    file = request.files["file"]
    tmp = os.path.join(DATA_DIR, "upload_tmp")
    file.save(tmp)
    generate_thumbnails(tmp)
    os.remove(tmp)
    return jsonify({"ok": True, "photoHash": current_photo_hash})
