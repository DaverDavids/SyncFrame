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

# ---------------------------------------------------------------------------
# Data directory
# ---------------------------------------------------------------------------
DATA_DIR = os.path.join(os.getcwd(), "data")
os.makedirs(DATA_DIR, exist_ok=True)

MOSQ_DIR = os.path.join(DATA_DIR, "mosq")
os.makedirs(MOSQ_DIR, exist_ok=True)

OTA_DIR = os.path.join(DATA_DIR, "ota")
os.makedirs(OTA_DIR, exist_ok=True)

# ---------------------------------------------------------------------------
# Give the mosquitto user ownership of MOSQ_DIR so it can write
# pid, log, conf and any other runtime files it needs.
# ---------------------------------------------------------------------------
try:
    import pwd as _pwd

    _mosq = _pwd.getpwnam("mosquitto")
    os.chown(MOSQ_DIR, _mosq.pw_uid, _mosq.pw_gid)
    logging.info(
        "Set %s ownership to mosquitto (%d:%d)", MOSQ_DIR, _mosq.pw_uid, _mosq.pw_gid
    )
except KeyError:
    logging.warning("mosquitto user not found - skipping chown of %s", MOSQ_DIR)
except Exception as _e:
    logging.warning("Could not chown %s to mosquitto: %s", MOSQ_DIR, _e)

# ---------------------------------------------------------------------------
# Seed default JPGs from the script directory into data/ on first run.
# ---------------------------------------------------------------------------
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for _fname in os.listdir(_SCRIPT_DIR):
    if _fname.lower().endswith(".jpg"):
        _src = os.path.join(_SCRIPT_DIR, _fname)
        _dst = os.path.join(DATA_DIR, _fname)
        if not os.path.exists(_dst):
            shutil.copy2(_src, _dst)
            logging.info("Seeded default image: %s -> %s", _src, _dst)

# ---------------------------------------------------------------------------
# Load or create configuration
# ---------------------------------------------------------------------------
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
    logging.info(f"Configuration file created at {CONFIG_PATH} with defaults.")
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
        logging.info(f"Configuration file {CONFIG_PATH} updated with missing defaults.")

# ---------------------------------------------------------------------------
# Secret key
# ---------------------------------------------------------------------------
OLD_SECRET_KEY = "syncframe-secret-key-change-me-in-production"

secret_key_value = config.get("server", "secret_key", fallback="").strip()
if not secret_key_value:
    secret_key_value = secrets.token_hex(32)
    config.set("server", "secret_key", secret_key_value)
    with open(CONFIG_PATH, "w") as cfgfile:
        config.write(cfgfile)
    logging.info("Generated new SECRET_KEY and saved to config.")

# ---------------------------------------------------------------------------
# Read configuration values
# ---------------------------------------------------------------------------
SERVER_HOST = config.get("server", "host", fallback=default_config["server"]["host"])
SERVER_PORT = config.getint(
    "server", "port", fallback=int(default_config["server"]["port"])
)
USE_HTTPS = config.getboolean(
    "server",
    "use_https",
    fallback=(default_config["server"]["use_https"].lower() == "true"),
)
CERTFILE = os.path.abspath(
    config.get("server", "certfile", fallback=default_config["server"]["certfile"])
)
KEYFILE = os.path.abspath(
    config.get("server", "keyfile", fallback=default_config["server"]["keyfile"])
)

if USE_HTTPS:
    try:
        ensure_certificates(CERTFILE, KEYFILE)
    except Exception as _cert_err:
        logging.error("Could not generate TLS certificates: %s", _cert_err)

_raw_prefix = config.get(
    "server", "url_prefix", fallback=default_config["server"]["url_prefix"]
).strip()
URL_PREFIX = "" if _raw_prefix in ("", "/") else "/" + _raw_prefix.strip("/")

USERNAME = config.get(
    "server", "username", fallback=default_config["server"]["username"]
)
PASSWORD = config.get(
    "server", "password", fallback=default_config["server"]["password"]
)

MQTT_HOST = config.get("mqtt", "host", fallback=default_config["mqtt"]["host"])
MQTT_PORT = config.getint("mqtt", "port", fallback=int(default_config["mqtt"]["port"]))
MQTT_TOPIC = config.get("mqtt", "topic", fallback=default_config["mqtt"]["topic"])
MQTT_USERNAME = config.get("mqtt", "username", fallback="")
MQTT_PASSWORD = config.get("mqtt", "password", fallback="")

WATCH_FILE = os.path.abspath(
    config.get(
        "watcher", "file_to_watch", fallback=default_config["watcher"]["file_to_watch"]
    )
)

IMAGE_MAX_WIDTH = config.getint("image", "max_width", fallback=1920)
IMAGE_MAX_HEIGHT = config.getint("image", "max_height", fallback=1080)
IMAGE_JPEG_QUALITY = config.getint("image", "jpeg_quality", fallback=85)

PHOTO_ETAG_PATH = os.path.join(DATA_DIR, "photo_upload_etag.txt")


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


photo_upload_etag = _load_photo_etag()
if not photo_upload_etag:
    photo_upload_etag = secrets.token_hex(8)
    _save_photo_etag(photo_upload_etag)

# ---------------------------------------------------------------------------
# SSE (Server-Sent Events) subscriber registry
# ---------------------------------------------------------------------------
_sse_subscribers: list[queue.SimpleQueue] = []
_sse_lock = threading.Lock()

connected_stream_clients: dict = {}   # keyed by MAC → {"response": <Response obj>, "resolution": (w,h), "hostname": str, "uptime": int, "compiled": str, "queue": queue.Queue}
_stream_lock = threading.Lock()


def _compute_crc32(path: str) -> str:
    val = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            val = zlib.crc32(chunk, val)
    return format(val & 0xFFFFFFFF, "08x")


current_photo_hash: str = _compute_crc32(WATCH_FILE) if os.path.exists(WATCH_FILE) else ""


def _sse_notify(message: str = "refresh"):
    """Notify all SSE subscribers of an event."""
    with _sse_lock:
        dead = []
        for q in _sse_subscribers:
            try:
                q.put_nowait(message)
            except Exception:
                dead.append(q)
        for q in dead:
            _sse_subscribers.remove(q)


def _push_photo_to_stream_clients():
    """Push new photo to all connected stream clients."""
    with _stream_lock:
        clients_snapshot = dict(connected_stream_clients)

    for mac, client in clients_snapshot.items():
        resolution = client.get("resolution", (800, 480))
        variant_file = WATCH_FILE.replace("photo.jpg", f"photo.{resolution[0]}x{resolution[1]}.jpg")
        if not os.path.exists(variant_file):
            variant_file = WATCH_FILE
        try:
            with open(variant_file, "rb") as f:
                jpeg_bytes = f.read()
            client["queue"].put_nowait(jpeg_bytes)
            logging.info(f"Pushed photo to stream client {mac}")
        except Exception as e:
            logging.warning(f"Failed to push photo to {mac}: {e}")


RESOLUTIONS = [
    (800, 480),
    (280, 240),
]

# Max output file size in KB per resolution. 0 = no limit.
RESOLUTION_MAX_KB = {
    (800, 480): 100,
    (280, 240): 20,
}

# ---------------------------------------------------------------------------
# OTA state
# ---------------------------------------------------------------------------
OTA_CLIENTS_PATH = os.path.join(OTA_DIR, "ota_clients.json")
OTA_FIRMWARE_DIR = OTA_DIR

_ota_lock = threading.Lock()


def _load_clients():
    if os.path.exists(OTA_CLIENTS_PATH):
        try:
            with open(OTA_CLIENTS_PATH, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def _save_clients(clients):
    with open(OTA_CLIENTS_PATH, "w") as f:
        json.dump(clients, f, indent=2)


# ---------------------------------------------------------------------------
# Firmware binary helpers
# ---------------------------------------------------------------------------

ESP_APP_DESC_MAGIC = 0xABCD5432

# Offsets within the esp_app_desc_t struct (relative to the magic word):
#   +0   magic_word   (4 bytes)  = 0xABCD5432
#   +4   secure_ver   (4 bytes)
#   +8   reserv1      (8 bytes)
#   +16  version[32]  <- firmware version string (legacy fallback)
#   +48  project_name (32 bytes)
#   +80  time[16]     <- compile time  e.g. "19:50:46"  (logged, not used as ID)
#   +96  date[16]     <- compile date  e.g. "Feb 11 2026" (logged, not used as ID)
_VER_REL_OFFSET = 16
_TIME_REL_OFFSET = 80
_DATE_REL_OFFSET = 96
_DESC_MIN_SIZE = 112  # must have at least date field fully present

_SFID_SENTINEL = b"SFID:"
_MONTH_MAP = {
    b"Jan": "01",
    b"Feb": "02",
    b"Mar": "03",
    b"Apr": "04",
    b"May": "05",
    b"Jun": "06",
    b"Jul": "07",
    b"Aug": "08",
    b"Sep": "09",
    b"Oct": "10",
    b"Nov": "11",
    b"Dec": "12",
}


def _find_app_desc_offset(data: bytes):
    """Scan data for the ESP_APP_DESC magic and return the offset, or None.

    Works for both app-only .bin files (magic at byte 32) and merged flash
    images where the app partition starts at an arbitrary flash offset
    (commonly 0x10000 = 65536).
    """
    magic_bytes = struct.pack("<I", ESP_APP_DESC_MAGIC)
    # Fast path: app-only binary — magic is at offset 32
    if data[32:36] == magic_bytes:
        return 32
    # Scan remaining file in 4-byte aligned steps
    pos = 36
    while pos <= len(data) - _DESC_MIN_SIZE:
        idx = data.find(magic_bytes, pos)
        if idx == -1:
            break
        if idx % 4 == 0:  # must be 4-byte aligned
            return idx
        pos = idx + 1
    return None


def extract_compile_id(data: bytes):
    """Extract compile ID from firmware binary.

    Scans for 'SFID:' sentinel and parses 'Mon DD YYYY HH:MM:SS' format.
    Returns (compile_id_str, None) on success, or (None, debug_info_str) on failure.
    debug_info_str describes what was found in the binary for diagnostics.
    """
    sidx = data.find(_SFID_SENTINEL)
    if sidx != -1 and sidx + 29 <= len(data):
        raw = data[sidx + 5 : sidx + 26]  # "Mar 17 2026 13:49:05" (21 chars)
        try:
            parts = raw.split()
            if len(parts) >= 4:
                month = _MONTH_MAP.get(parts[0], "00")
                day = parts[1].decode() if isinstance(parts[1], bytes) else parts[1]
                year = parts[2].decode() if isinstance(parts[2], bytes) else parts[2]
                time_str = (
                    parts[3].decode() if isinstance(parts[3], bytes) else parts[3]
                ).replace("\x00", "").strip()
                time_part = time_str.replace(":", "")
                if len(day) == 1:
                    day = "0" + day
                return f"{year}{month}{day}-{time_part}", None
            else:
                return None, f"SFID sentinel found at offset {sidx} but could not parse date/time from: {raw!r}"
        except Exception as e:
            return None, f"SFID sentinel found at offset {sidx} but parse error: {e}, raw={raw!r}"
    else:
        debug_parts = [f"SFID sentinel not found in {len(data)}-byte binary"]
        desc_off = _find_app_desc_offset(data)
        if desc_off is None:
            debug_parts.append("esp_app_desc_t magic not found either")
        else:
            off = desc_off + _VER_REL_OFFSET
            version_bytes = data[off : off + 32]
            null_pos = version_bytes.find(b"\x00")
            if null_pos >= 0:
                version_bytes = version_bytes[:null_pos]
            version_str = version_bytes.decode("utf-8", errors="ignore").strip()
            debug_parts.append(f"app_desc found at offset {desc_off}, version field='{version_str}'")
            snippet_start = max(0, desc_off - 64)
            snippet = data[snippet_start : desc_off + 128]
            debug_parts.append(f"binary snippet [{snippet_start}:{snippet_start+len(snippet)}]: {snippet!r}")
        return None, " | ".join(debug_parts)


# ---------------------------------------------------------------------------
# Flask app
# ---------------------------------------------------------------------------
app = Flask(__name__)
app.secret_key = secret_key_value
bp = Blueprint("syncframe", __name__)
ALLOWED_EXTENSIONS = {"jpg", "jpeg", "png", "heic", "webp", "gif"}
ALLOWED_FIRMWARE_EXTENSIONS = {"bin"}
UPLOAD_FOLDER = DATA_DIR


class DualKeySessionInterface(SecureCookieSessionInterface):
    def open_session(self, app, request):
        sess = super().open_session(app, request)
        cookie_name = self.get_cookie_name(app)
        raw_cookie = request.cookies.get(cookie_name)
        if raw_cookie and not sess.get("authenticated"):
            try:
                s = URLSafeTimedSerializer(OLD_SECRET_KEY, salt="cookie-session")
                data = s.loads(raw_cookie)
                logging.info("Session migrated from old secret key to new key.")
                return self.session_class(data)
            except BadSignature:
                pass
        return sess


app.session_interface = DualKeySessionInterface()


def check_auth(username, password):
    return username == USERNAME and password == PASSWORD


def create_mqtt_password_file():
    """Always regenerate the pwfile from current config so credential changes take effect."""
    pwfile = os.path.join(MOSQ_DIR, "pwfile")
    if not MQTT_USERNAME or not MQTT_PASSWORD:
        logging.warning(
            "MQTT username or password not set - skipping pwfile generation"
        )
        return
    try:
        subprocess.check_call(
            ["mosquitto_passwd", "-c", "-b", pwfile, MQTT_USERNAME, MQTT_PASSWORD],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        os.chmod(pwfile, 0o600)
        try:
            import pwd

            mosq = pwd.getpwnam("mosquitto")
            os.chown(pwfile, mosq.pw_uid, mosq.pw_gid)
        except Exception as e:
            logging.warning(f"Could not change pwfile ownership: {e}")
            os.chmod(pwfile, 0o644)
        logging.info(f"MQTT password file (re)generated for user {MQTT_USERNAME}")
    except Exception as e:
        logging.error(f"Failed to create password file: {e}")


def requires_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if session.get("authenticated"):
            return f(*args, **kwargs)
        auth = request.authorization
        if auth and check_auth(auth.username, auth.password):
            session.permanent = True
            session["authenticated"] = True
            app.permanent_session_lifetime = timedelta(days=3650)
            return f(*args, **kwargs)
        return Response(
            "Authentication required",
            401,
            {"WWW-Authenticate": 'Basic realm="Login Required"'},
        )

    return decorated


def generate_self_signed_cert_py(
    certfile, keyfile, common_name="localhost", days_valid=3650
):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name(
        [
            x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
            x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, "State"),
            x509.NameAttribute(NameOID.LOCALITY_NAME, "Locality"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "SyncFrame"),
            x509.NameAttribute(NameOID.COMMON_NAME, common_name),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.utcnow() - timedelta(days=1))
        .not_valid_after(datetime.utcnow() + timedelta(days=days_valid))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName("syncframe"), x509.DNSName("localhost")]),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    with open(keyfile, "wb") as f:
        f.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    os.chmod(keyfile, 0o600)
    with open(certfile, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))


def generate_self_signed_cert_openssl(
    certfile, keyfile, common_name="localhost", days_valid=3650
):
    cmd = [
        "openssl",
        "req",
        "-x509",
        "-nodes",
        "-newkey",
        "rsa:2048",
        "-keyout",
        keyfile,
        "-out",
        certfile,
        "-days",
        str(days_valid),
        "-subj",
        f"/CN={common_name}",
    ]
    try:
        subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(keyfile, 0o600)
    except Exception as e:
        logging.error("OpenSSL certificate generation failed: %s", e)
        raise


def ensure_certificates(certfile, keyfile):
    if os.path.exists(certfile) and os.path.exists(keyfile):
        logging.info("Certificate and key already exist: %s, %s", certfile, keyfile)
        return
    for path in (certfile, keyfile):
        d = os.path.dirname(path)
        if d and not os.path.exists(d):
            os.makedirs(d, exist_ok=True)
    logging.info(
        "Generating self-signed certificate and key at %s and %s", certfile, keyfile
    )
    try:
        if CRYPTOGRAPHY_AVAILABLE:
            generate_self_signed_cert_py(certfile, keyfile)
            logging.info("Self-signed certificate generated using cryptography.")
        else:
            generate_self_signed_cert_openssl(certfile, keyfile)
            logging.info("Self-signed certificate generated using openssl CLI.")
    except Exception as e:
        logging.error("Failed to generate self-signed certificate: %s", e)
        raise


def fix_image_orientation(image):
    try:
        exif = image._getexif()
        if not exif:
            return image
        orientation = exif.get(274)
        if orientation == 3:
            image = image.rotate(180, expand=True)
        elif orientation == 6:
            image = image.rotate(270, expand=True)
        elif orientation == 8:
            image = image.rotate(90, expand=True)
        return image
    except Exception:
        return image


def generate_mqtt_certificates():
    os.makedirs(MOSQ_DIR, exist_ok=True)
    ca_key = os.path.join(MOSQ_DIR, "ca.key")
    ca_crt = os.path.join(MOSQ_DIR, "ca.crt")
    server_key = os.path.join(MOSQ_DIR, "server.key")
    server_crt = os.path.join(MOSQ_DIR, "server.crt")
    server_csr = os.path.join(MOSQ_DIR, "server.csr")

    if (
        os.path.exists(ca_crt)
        and os.path.exists(server_crt)
        and os.path.exists(server_key)
    ):
        logging.info("MQTT certificates already exist in %s", MOSQ_DIR)
        return

    logging.info("Generating MQTT certificates in %s...", MOSQ_DIR)
    try:
        subprocess.check_call(
            ["openssl", "genrsa", "-out", ca_key, "2048"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            [
                "openssl",
                "req",
                "-new",
                "-x509",
                "-days",
                "3650",
                "-key",
                ca_key,
                "-out",
                ca_crt,
                "-subj",
                "/CN=MQTT-CA",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            ["openssl", "genrsa", "-out", server_key, "2048"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            [
                "openssl",
                "req",
                "-new",
                "-key",
                server_key,
                "-out",
                server_csr,
                "-subj",
                "/CN=localhost",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.check_call(
            [
                "openssl",
                "x509",
                "-req",
                "-in",
                server_csr,
                "-CA",
                ca_crt,
                "-CAkey",
                ca_key,
                "-CAcreateserial",
                "-out",
                server_crt,
                "-days",
                "3650",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        os.chmod(ca_key, 0o600)
        os.chmod(server_key, 0o600)
        try:
            import pwd

            mosq = pwd.getpwnam("mosquitto")
            for f in (ca_crt, server_crt, server_key):
                os.chown(f, mosq.pw_uid, mosq.pw_gid)
        except Exception as e:
            logging.warning(f"Could not change certificate ownership: {e}")
        if os.path.exists(server_csr):
            os.remove(server_csr)
        logging.info("MQTT certificates generated successfully in %s", MOSQ_DIR)
    except Exception as e:
        logging.error(f"Failed to generate MQTT certificates: {e}")
        raise


def generate_thumbnails(source_img=None):
    try:
        if source_img is None:
            if not os.path.exists(WATCH_FILE):
                logging.warning("Cannot generate thumbnails - photo.jpg does not exist")
                return
            source_img = Image.open(WATCH_FILE)
            source_img = fix_image_orientation(source_img)
        for w, h in RESOLUTIONS:
            try:
                thumb = source_img.copy()
                thumb.thumbnail((w, h), Image.LANCZOS)
                out_path = WATCH_FILE.replace("photo.jpg", f"photo.{w}x{h}.jpg")
                max_kb = RESOLUTION_MAX_KB.get((w, h), 0)
                quality = 75
                while True:
                    buf = io.BytesIO()
                    thumb.save(buf, format="JPEG", quality=quality, optimize=True)
                    if max_kb == 0 or buf.tell() <= max_kb * 1024 or quality <= 10:
                        break
                    quality -= 5
                with open(out_path, "wb") as f:
                    f.write(buf.getvalue())
                if max_kb > 0:
                    logging.info("Thumbnail %dx%d: %d bytes at quality=%d", w, h, buf.tell(), quality)
                logging.info("Thumbnail saved: %s at size %s", out_path, thumb.size)
            except Exception as e:
                logging.error("Error generating %dx%d thumbnail: %s", w, h, e)
    except Exception as e:
        logging.error("Error in generate_thumbnails: %s", e)


class Watcher:
    FILE_TO_WATCH = WATCH_FILE

    def __init__(self):
        self.observer = Observer()
        self.last_md5 = self.calculate_md5()
        self.last_notification = 0
        self.debounce_ms = 1000

    def calculate_md5(self):
        hash_md5 = hashlib.md5()
        try:
            with open(self.FILE_TO_WATCH, "rb") as f:
                for chunk in iter(lambda: f.read(4096), b""):
                    hash_md5.update(chunk)
        except FileNotFoundError:
            return None
        return hash_md5.hexdigest()

    def run(self):
        event_handler = Handler(self)
        watch_dir = os.path.dirname(os.path.abspath(self.FILE_TO_WATCH)) or "."
        self.observer.schedule(event_handler, watch_dir, recursive=False)
        self.observer.start()
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            self.observer.stop()
        self.observer.join()


class Handler(FileSystemEventHandler):
    def __init__(self, watcher):
        self.watcher = watcher

    def on_modified(self, event):
        try:
            event_path = os.path.abspath(event.src_path)
            watched_path = os.path.abspath(self.watcher.FILE_TO_WATCH)
        except Exception:
            return None
        if event.is_directory or event_path != watched_path:
            return None
        current_time = int(time.time() * 1000)
        if current_time - self.watcher.last_notification < self.watcher.debounce_ms:
            return None
        current_md5 = self.watcher.calculate_md5()
        if current_md5 != self.watcher.last_md5:
            self.watcher.last_md5 = current_md5
            self.watcher.last_notification = current_time
            logging.info(f"File {event.src_path} has been modified")
            send_mqtt_message("refresh")
            _sse_notify("refresh")
            global current_photo_hash
            current_photo_hash = _compute_crc32(WATCH_FILE)
            _push_photo_to_stream_clients()


def send_mqtt_message(message):
    logging.info("Sending MQTT message: %s", message)
    client = mqtt.Client()
    ca_crt = os.path.join(MOSQ_DIR, "ca.crt")
    if os.path.exists(ca_crt):
        try:
            client.tls_set(ca_certs=ca_crt, tls_version=ssl.PROTOCOL_TLSv1_2)
            client.tls_insecure_set(True)
            logging.info("MQTT TLS enabled")
        except Exception as e:
            logging.warning(f"Could not enable MQTT TLS: {e}")
    if MQTT_USERNAME and MQTT_PASSWORD:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    try:
        client.connect(MQTT_HOST, MQTT_PORT, 60)
        client.publish(MQTT_TOPIC, message, qos=0, retain=False)
    except Exception as e:
        logging.error(f"Failed to connect to MQTT broker: {e}")
    finally:
        try:
            client.disconnect()
        except Exception:
            pass


def desaturate_image():
    try:
        img = Image.open(WATCH_FILE)
        converter = ImageEnhance.Color(img)
        img = converter.enhance(0.66)
        changed_path = os.path.join(DATA_DIR, "photo-changed.jpg")
        img.save(changed_path)
        logging.info("Desaturated %s by 34%% and saved to %s", WATCH_FILE, changed_path)
        finalize_changes(img)
    except Exception as e:
        logging.error(f"Error desaturating image: {e}")


def finalize_changes(img=None):
    try:
        changed_path = os.path.join(DATA_DIR, "photo-changed.jpg")
        shutil.copy(changed_path, WATCH_FILE)
        logging.info("Copied %s to %s", changed_path, WATCH_FILE)
        generate_thumbnails(source_img=img)
    except Exception as e:
        logging.error(f"Error in finalize_changes: {e}")


def schedule_desaturation():
    schedule.every().day.at("03:00").do(desaturate_image)
    while True:
        schedule.run_pending()
        time.sleep(1)


def write_mosquitto_conf():
    conf_path = os.path.join(MOSQ_DIR, "mosquitto.conf")
    if os.path.exists(conf_path):
        return
    content = f"""# Auto-generated by syncframe-server.py
# Edit this file to customise Mosquitto. It will not be overwritten once created.

per_listener_settings true
pid_file {os.path.join(MOSQ_DIR, 'mosquitto.pid')}
persistence false
log_dest file {os.path.join(MOSQ_DIR, 'mosquitto.log')}

listener 9368
allow_anonymous false
password_file {os.path.join(MOSQ_DIR, 'pwfile')}

cafile {os.path.join(MOSQ_DIR, 'ca.crt')}
certfile {os.path.join(MOSQ_DIR, 'server.crt')}
keyfile {os.path.join(MOSQ_DIR, 'server.key')}
tls_version tlsv1.2
"""
    with open(conf_path, "w") as f:
        f.write(content)
    logging.info("Generated mosquitto.conf at %s", conf_path)


# ---------------------------------------------------------------------------
# Flask routes
# ---------------------------------------------------------------------------


@bp.route("/", methods=["GET"])
@requires_auth
def index():
    last_updated = None
    if os.path.exists(WATCH_FILE):
        last_updated = int(os.path.getmtime(WATCH_FILE) * 1000)
    html_content = """
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>SyncFrame Photo Viewer</title>

        <link rel="manifest" href="{{ url_for('syncframe.serve_static', filename='manifest.webmanifest') }}">
        <link rel="icon" type="image/x-icon" href="{{ url_for('syncframe.serve_static', filename='favicon.ico') }}">
        <link rel="icon" type="image/png" sizes="16x16" href="{{ url_for('syncframe.serve_static', filename='favicon-16x16.png') }}">
        <link rel="icon" type="image/png" sizes="32x32" href="{{ url_for('syncframe.serve_static', filename='favicon-32x32.png') }}">
        <link rel="apple-touch-icon" sizes="180x180" href="{{ url_for('syncframe.serve_static', filename='apple-touch-icon.png') }}">
        <link rel="icon" type="image/png" sizes="192x192" href="{{ url_for('syncframe.serve_static', filename='icon-192x192.png') }}">
        <link rel="icon" type="image/png" sizes="512x512" href="{{ url_for('syncframe.serve_static', filename='icon-512x512.png') }}">
        <meta name="apple-mobile-web-app-capable" content="yes">
        <meta name="apple-mobile-web-app-title" content="SyncFrame">
    <style>
        body {
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            background:
                radial-gradient(circle at 20% 20%, rgba(255, 0, 150, 0.8) 0%, transparent 50%),
                radial-gradient(circle at 80% 80%, rgba(0, 255, 100, 0.7) 0%, transparent 50%),
                radial-gradient(circle at 80% 20%, rgba(0, 200, 255, 0.8) 0%, transparent 50%),
                radial-gradient(circle at 20% 80%, rgba(255, 100, 0, 0.7) 0%, transparent 50%),
                linear-gradient(135deg, #1a0033 0%, #003333 100%);
            background-attachment: fixed;
            background-size: 150% 150%;
            font-family: Arial, sans-serif;
            text-align: center;
            color: #ffffff;
            animation: gradientShift 15s ease infinite;
            overflow-x: hidden;
        }
        @keyframes gradientShift {
            0% { background-position: 0% 50%; }
            50% { background-position: 100% 50%; }
            100% { background-position: 0% 50%; }
        }
        h1 { color: #ffffff; text-shadow: 0 2px 10px rgba(0,0,0,0.7); font-size: 2.5em; margin-bottom: 30px; }
        form { margin-bottom: 30px; }
        .container { display: flex; flex-wrap: wrap; justify-content: space-around; align-items: center; gap: 60px; }
        .image-container {
            flex: 1; min-width: 300px; max-width: 45%; text-align: center;
            background: #000000; padding: 0; margin-top: 50px; border-radius: 4px;
            border: 15px solid;
            border-image: linear-gradient(145deg, #8b6f47, #d4a574, #c4956a, #8b6f47, #6a5537) 1;
            box-shadow: inset 0 0 30px rgba(0,0,0,0.8), 0 10px 40px rgba(0,0,0,0.8),
                        0 0 0 3px #4a3a2a, 0 0 0 4px #2a1a0a;
            position: relative; aspect-ratio: 16/9; overflow: visible;
        }
        .image-container img {
            width: 100%; height: 100%; object-fit: contain; border: none;
            border-radius: 2px; position: relative; z-index: 1; display: block;
        }
        .image-container h2 {
            position: absolute; top: -60px; left: -40px; right: -40px; bottom: -40px;
            margin: 0; padding: 15px 20px; padding-bottom: 60px; font-size: 1.3em;
            color: #ffffff; text-shadow: 0 2px 8px rgba(0,0,0,0.6);
            background: rgba(0,0,0,0.4); backdrop-filter: blur(10px);
            border-radius: 12px; z-index: -1;
            display: flex; align-items: flex-start; justify-content: center;
        }
        input[type="file"] {
            margin-bottom: 20px; background: rgba(255,255,255,0.15); color: #ffffff;
            border: 1px solid rgba(255,255,255,0.3); padding: 10px 20px;
            border-radius: 6px; backdrop-filter: blur(10px); cursor: pointer;
        }
        input[type="file"]::file-selector-button {
            background: rgba(255,255,255,0.2); color: #ffffff; border: none;
            padding: 5px 15px; border-radius: 4px; cursor: pointer; margin-right: 10px;
        }
        input[type="file"]::file-selector-button:hover { background: rgba(255,255,255,0.3); }
        button {
            background: rgba(255,255,255,0.15); color: #ffffff;
            border: 1px solid rgba(255,255,255,0.3); padding: 10px 20px;
            border-radius: 6px; cursor: pointer; backdrop-filter: blur(10px);
            font-size: 1em; transition: background 0.3s ease;
        }
        button:hover { background: rgba(255,255,255,0.25); }
        @media (max-width: 768px) {
            .container { flex-direction: column; }
            .image-container { max-width: 100%; }
            h1 { font-size: 2em; }
        }
        footer { margin-bottom: 0; }
        footer p { margin-bottom: 0; }
    </style>
    </head>
    <body>
        <h1>SyncFrame Photo Viewer</h1>
        <form action="{{ url_for('syncframe.upload_file') }}" method="POST" enctype="multipart/form-data">
            <input type="file" name="file" accept="image/*" required>
            <button type="submit">Upload Photo</button>
        </form>
        <div class="container">
            <div class="image-container">
                <h2>Uploaded Photo</h2>
                <img id="uploaded-photo" src="" alt="Uploaded Photo" style="display: none;">
            </div>
            <div class="image-container">
                <h2>Current Photo</h2>
                <img id="current-photo" src="{{ url_for('syncframe.serve_photo') }}" alt="Current Photo">
            </div>
        </div>
        <script>
            const fileInput    = document.querySelector('input[type="file"]');
            const uploadButton = document.querySelector('button[type="submit"]');
            const uploadedPhoto = document.getElementById('uploaded-photo');
            fileInput.addEventListener('change', function(event) {
                const file = event.target.files[0];
                if (file) {
                    const reader = new FileReader();
                    reader.onload = function(e) {
                        uploadedPhoto.src = e.target.result;
                        uploadedPhoto.style.display = 'block';
                    };
                    reader.readAsDataURL(file);
                    uploadButton.style.background  = 'rgba(0,255,100,0.8)';
                    uploadButton.style.border      = '2px solid rgba(0,255,100,1)';
                    uploadButton.style.transform   = 'scale(1.1)';
                    uploadButton.style.boxShadow   = '0 0 20px rgba(0,255,100,0.6)';
                    uploadButton.textContent       = '\\u2713 Click to Upload!';
                } else {
                    uploadedPhoto.src = '';
                    uploadedPhoto.style.display   = 'none';
                    uploadButton.style.background  = 'rgba(255,255,255,0.15)';
                    uploadButton.style.border      = '1px solid rgba(255,255,255,0.3)';
                    uploadButton.style.transform   = 'scale(1)';
                    uploadButton.style.boxShadow   = 'none';
                    uploadButton.textContent       = 'Upload Photo';
                }
            });
        </script>
        <footer style="margin-top:10px;padding:5px;color:rgba(255,255,255,0.7);font-size:0.9em;">
            <p id="last-updated">
                {% if last_updated %}Last updated: <span id='update-time'></span>{% else %}No photo uploaded yet{% endif %}
            </p>
        </footer>
        <script>
            const lastUpdated = {{ last_updated if last_updated else 'null' }};
            if (lastUpdated) {
                const date = new Date(lastUpdated);
                document.getElementById('update-time').textContent = date.toLocaleString('en-US', {
                    month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit', hour12: true
                });
            }
        </script>
    </body>
    </html>
    """
    return render_template_string(html_content, last_updated=last_updated)


@bp.route("/upload", methods=["POST"])
@requires_auth
def upload_file():
    if "file" not in request.files:
        return redirect(request.url)
    file = request.files["file"]
    if not file or not allowed_file(file.filename):
        return "File not allowed", 400
    target_path = WATCH_FILE
    ext = file.filename.rsplit(".", 1)[1].lower()
    try:
        data = file.read()
        buf = BytesIO(data)
        if ext == "heic":
            try:
                heif_file = pillow_heif.read_heif(buf.getvalue())
                img = Image.frombytes(
                    heif_file.mode,
                    heif_file.size,
                    heif_file.data,
                    "raw",
                    heif_file.mode,
                    heif_file.stride,
                )
            except Exception as e:
                logging.error("HEIC conversion failed: %s", e)
                return "HEIC conversion failed", 500
        elif ext == "gif":
            try:
                buf.seek(0)
                img = Image.open(buf)
                first_frame = next(ImageSequence.Iterator(img))
                img = (
                    first_frame.convert("RGB")
                    if first_frame.mode in ("RGBA", "P", "LA")
                    else first_frame
                )
            except Exception as e:
                logging.error("GIF processing failed: %s", e)
                return "GIF processing failed", 500
        else:
            try:
                buf.seek(0)
                img = Image.open(buf)
                img = fix_image_orientation(img)
            except Exception as e:
                logging.error("Image open failed: %s", e)
                return "Image open failed", 500
        if img.mode not in ("RGB", "L"):
            img = img.convert("RGB")
        max_size = (IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT)
        img.thumbnail(max_size, Image.LANCZOS)
        img.save(target_path, format="JPEG", quality=IMAGE_JPEG_QUALITY, optimize=True)
        logging.info(
            "Uploaded image resized to max %s and saved to %s", max_size, target_path
        )
        global photo_upload_etag
        photo_upload_etag = secrets.token_hex(8)
        _save_photo_etag(photo_upload_etag)
        generate_thumbnails(source_img=img)
        global current_photo_hash
        current_photo_hash = _compute_crc32(target_path)
        _push_photo_to_stream_clients()
        return redirect(url_for("syncframe.index"))
    except Exception as e:
        logging.error("Upload failed: %s", e)
        return "Upload failed", 500


@bp.route("/photo.jpg")
@requires_auth
def serve_photo():
    if os.path.exists(WATCH_FILE):
        resp = send_from_directory(
            os.path.dirname(os.path.abspath(WATCH_FILE)) or ".",
            os.path.basename(WATCH_FILE),
        )
        resp.headers["ETag"] = photo_upload_etag
        return resp
    return "No photo available", 404


@bp.route("/photo.<int:w>x<int:h>.jpg")
@requires_auth
def serve_photo_variant(w, h):
    if (w, h) not in RESOLUTIONS:
        return "Resolution not supported", 404
    variant_file = WATCH_FILE.replace("photo.jpg", f"photo.{w}x{h}.jpg")
    if os.path.exists(variant_file):
        resp = send_from_directory(
            os.path.dirname(os.path.abspath(variant_file)) or ".",
            os.path.basename(variant_file),
        )
        resp.headers["ETag"] = photo_upload_etag
        return resp
    if os.path.exists(WATCH_FILE):
        logging.info(
            "Variant %dx%d missing - generating all thumbnails on-the-fly", w, h
        )
        generate_thumbnails()
        if os.path.exists(variant_file):
            resp = send_from_directory(
                os.path.dirname(os.path.abspath(variant_file)) or ".",
                os.path.basename(variant_file),
            )
            resp.headers["ETag"] = photo_upload_etag
            return resp
    return "No photo available - upload an image first", 404


@bp.route("/static/<path:filename>")
def serve_static(filename):
    return send_from_directory("static", filename)


@bp.route("/events")
def sse_stream():
    """Server-Sent Events endpoint — clients subscribe here for live refresh notifications."""
    def generate():
        q = queue.SimpleQueue()
        with _sse_lock:
            _sse_subscribers.append(q)
        try:
            yield "data: connected\n\n"
            while True:
                try:
                    msg = q.get(timeout=25)
                    yield f"data: {msg}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        finally:
            with _sse_lock:
                try:
                    _sse_subscribers.remove(q)
                except ValueError:
                    pass

    return Response(
        generate(),
        mimetype="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )


@bp.route("/stream")
@requires_auth
def stream():
    """MJPEG-style streaming endpoint for client device communication."""
    hostname = (request.headers.get("X-SF-Hostname") or "").strip()
    mac_raw = (request.headers.get("X-SF-MAC") or "").strip().upper()
    mac = mac_raw.replace(":", "")  # strip colons
    uptime = (request.headers.get("X-SF-Uptime") or "").strip()
    compiled = (request.headers.get("X-SF-Compiled") or "").strip()
    photo_hash = (request.headers.get("X-SF-Photo-Hash") or "").strip()
    resolution_str = (request.headers.get("X-SF-Resolution") or "").strip()
    resolution = (800, 480)  # default
    if resolution_str:
        try:
            parts = resolution_str.lower().split("x")
            if len(parts) == 2:
                resolution = (int(parts[0]), int(parts[1]))
        except Exception:
            pass

    logging.info(f"Stream connect: mac={mac} hostname={hostname} resolution={resolution} compiled={compiled}")

    with _stream_lock:
        connected_stream_clients[mac] = {
            "resolution": resolution,
            "hostname": hostname,
            "uptime": uptime,
            "compiled": compiled,
            "queue": queue.SimpleQueue(),
        }

    _update_ota_client(mac, compiled, resolution, hostname, uptime)

    def generate():
        nonlocal mac
        last_push = time.time()
        q = connected_stream_clients[mac]["queue"]
        
        # OTA check: before entering keepalive loop, check for pending firmware
        clients = _load_clients()
        client_data = clients.get(hostname)
        if client_data and client_data.get("firmware"):
            fw_path = os.path.join(OTA_FIRMWARE_DIR, client_data["firmware"])
            fw_compile_id = client_data.get("fw_compile_id", "")
            if os.path.exists(fw_path) and fw_compile_id != compiled:
                try:
                    with open(fw_path, "rb") as f:
                        fw_data = f.read()
                    fw_len = len(fw_data)
                    yield f"--frame\r\nContent-Type: application/octet-stream\r\nX-SF-Frame-Type: ota\r\nContent-Length: {fw_len}\r\n\r\n".encode()
                    yield fw_data
                    yield b"--frame--\r\n"
                    logging.info(f"Stream OTA pushed to {hostname}: {client_data['firmware']}")
                    with _stream_lock:
                        if mac in connected_stream_clients:
                            del connected_stream_clients[mac]
                    return
                except Exception as e:
                    logging.warning(f"Failed to push OTA to {hostname}: {e}")
        
        # Photo check: push current photo if it differs from client's hash (or client has no hash)
        if current_photo_hash and photo_hash != current_photo_hash:
            variant_file = WATCH_FILE.replace("photo.jpg", f"photo.{resolution[0]}x{resolution[1]}.jpg")
            if not os.path.exists(variant_file):
                variant_file = WATCH_FILE
            try:
                with open(variant_file, "rb") as f:
                    jpeg_bytes = f.read()
                yield f"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: {len(jpeg_bytes)}\r\n\r\n".encode()
                yield jpeg_bytes
                last_push = time.time()
                logging.info(f"Stream initial photo pushed to {mac}")
            except Exception as e:
                logging.warning(f"Failed to push initial photo to {mac}: {e}")
        
        try:
            while True:
                try:
                    item = q.get(timeout=1.0)
                except queue.Empty:
                    if time.time() - last_push >= 60:
                        yield b"--frame\r\n\r\n"
                        last_push = time.time()
                    continue

                if isinstance(item, bytes):
                    if item == b"_keepalive_":
                        yield b"--frame\r\n\r\n"
                        last_push = time.time()
                    else:
                        yield f"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: {len(item)}\r\n\r\n".encode()
                        yield item
                        last_push = time.time()
        except GeneratorExit:
            pass
        finally:
            with _stream_lock:
                if mac in connected_stream_clients:
                    del connected_stream_clients[mac]
            logging.info(f"Stream disconnect: mac={mac}")

    return Response(
        generate(),
        mimetype="multipart/x-mixed-replace; boundary=frame",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


def _update_ota_client(mac, compiled, resolution, hostname, uptime):
    """Update OTA clients JSON with stream client info. Uses hostname as key, preserves existing fields."""
    clients = _load_clients()
    # Use hostname as key (consistent with OTA admin page)
    if hostname in clients:
        # Merge with existing record, preserve firmware fields
        clients[hostname]["compiled"] = compiled
        clients[hostname]["resolution"] = f"{resolution[0]}x{resolution[1]}"
        clients[hostname]["uptime"] = uptime
        clients[hostname]["last_seen"] = int(time.time())
    else:
        clients[hostname] = {
            "hostname": hostname,
            "compiled": compiled,
            "resolution": f"{resolution[0]}x{resolution[1]}",
            "uptime": uptime,
            "last_seen": int(time.time()),
        }
    _save_clients(clients)


def allowed_file(filename):
    return "." in filename and filename.rsplit(".", 1)[1].lower() in ALLOWED_EXTENSIONS


def allowed_firmware(filename):
    return (
        "." in filename
        and filename.rsplit(".", 1)[1].lower() in ALLOWED_FIRMWARE_EXTENSIONS
    )


# ---------------------------------------------------------------------------
# OTA public endpoint
# ---------------------------------------------------------------------------


@bp.route("/ota")
def ota_check():
    hostname = (request.headers.get("X-SF-Hostname") or "").strip()
    mac = (request.headers.get("X-SF-MAC") or "").strip().upper()
    compiled = (request.headers.get("X-SF-Compiled") or "").strip()
    uptime = (request.headers.get("X-SF-Uptime") or "").strip()

    now_iso = datetime.now(timezone.utc).isoformat()

    with _ota_lock:
        clients = _load_clients()

        # --- entry lookup ---
        mac_match = None
        hostname_match = None

        if mac:
            for k, v in clients.items():
                if v.get("mac") == mac:
                    mac_match = k
                    break

        for k, v in clients.items():
            if v.get("hostname", k) == hostname and v.get("mac", "") in (
                "",
                "0000000000",
            ):
                hostname_match = k
                break

        matched_key = None

        if mac_match is not None:
            matched_key = mac_match
        elif hostname_match is not None:
            matched_key = hostname_match
            if mac:
                existing_mac = clients[hostname_match].get("mac", "")
                if (
                    existing_mac
                    and existing_mac not in ("", "0000000000")
                    and existing_mac != mac
                ):
                    new_key = f"{hostname}--{mac}"
                    clients[new_key] = {
                        "hostname": hostname,
                        "mac": mac,
                        "label": new_key,
                        "last_seen": now_iso,
                        "compiled": compiled or None,
                        "uptime": uptime or None,
                        "firmware": None,
                        "fw_compile_id": None,
                        "last_flashed": None,
                    }
                    matched_key = new_key
                else:
                    clients[hostname_match]["mac"] = mac
        else:
            matched_key = hostname
            clients[matched_key] = {
                "hostname": hostname,
                "mac": mac,
                "label": hostname,
                "last_seen": now_iso,
                "compiled": compiled or None,
                "uptime": uptime or None,
                "firmware": None,
                "fw_compile_id": None,
                "last_flashed": None,
            }

        if matched_key not in clients:
            clients[matched_key] = {
                "hostname": hostname,
                "mac": mac,
                "label": hostname,
                "last_seen": now_iso,
                "compiled": compiled or None,
                "uptime": uptime or None,
                "firmware": None,
                "fw_compile_id": None,
                "last_flashed": None,
            }

        clients[matched_key]["last_seen"] = now_iso
        if compiled:
            clients[matched_key]["compiled"] = compiled
        if uptime:
            clients[matched_key]["uptime"] = uptime

        entry = clients[matched_key]
        fw_compile_id = entry.get("fw_compile_id")
        firmware_file = entry.get("firmware")

        if fw_compile_id and compiled == fw_compile_id:
            entry["firmware"] = None
            entry["fw_compile_id"] = None
            entry["last_flashed"] = now_iso
            _save_clients(clients)
            logging.info(
                "OTA: %s already at %s — cleared pending firmware",
                matched_key,
                compiled,
            )
            return Response("", status=204)

        if fw_compile_id and compiled != fw_compile_id and firmware_file:
            fw_path = os.path.join(OTA_FIRMWARE_DIR, firmware_file)
            if os.path.exists(fw_path):
                _save_clients(clients)
                logging.info(
                    "OTA: sending %s to %s (compiled=%s)",
                    firmware_file,
                    matched_key,
                    compiled,
                )
                return send_file(fw_path, mimetype="application/octet-stream")

        _save_clients(clients)

    return Response("", status=204)


# ---------------------------------------------------------------------------
# OTA Admin page
# ---------------------------------------------------------------------------

ADMIN_HTML = """
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SyncFrame OTA Admin</title>
<style>
  :root {
    --bg: #0d1117; --surface: #161b22; --border: #30363d;
    --accent: #58a6ff; --accent2: #3fb950; --warn: #f85149;
    --text: #c9d1d9; --text-dim: #8b949e;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; padding: 24px; }
  h1 { font-size: 1.6em; margin-bottom: 6px; }
  h1 span { color: var(--accent); }
  .subtitle { color: var(--text-dim); font-size: 0.9em; margin-bottom: 28px; }
  .subtitle a { color: var(--accent); text-decoration: none; }
  .card { background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 20px; margin-bottom: 24px; }
  .card h2 { margin-bottom: 16px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.05em; font-size: 0.8em; }
  table { width: 100%; border-collapse: collapse; font-size: 0.9em; }
  th { text-align: left; padding: 8px 12px; color: var(--text-dim); border-bottom: 1px solid var(--border); font-weight: 500; font-size: 0.8em; text-transform: uppercase; }
  td { padding: 10px 12px; border-bottom: 1px solid var(--border); vertical-align: middle; }
  tr:last-child td { border-bottom: none; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 0.75em; font-weight: 600; }
  .badge-green  { background: rgba(63,185,80,0.15);   color: var(--accent2); border: 1px solid rgba(63,185,80,0.3); }
  .badge-gray   { background: rgba(139,148,158,0.15); color: var(--text-dim); border: 1px solid rgba(139,148,158,0.2); }
  .badge-blue   { background: rgba(88,166,255,0.15);  color: var(--accent);  border: 1px solid rgba(88,166,255,0.3); }
  .badge-yellow { background: rgba(210,153,34,0.15);  color: #e3b341;        border: 1px solid rgba(210,153,34,0.3); }
  input[type=text], input[type=file] {
    background: var(--bg); color: var(--text); border: 1px solid var(--border);
    border-radius: 6px; padding: 7px 12px; font-size: 0.9em; width: 100%;
  }
  input[type=text]:focus { outline: none; border-color: var(--accent); }
  .btn {
    display: inline-block; padding: 7px 16px; border-radius: 6px; border: 1px solid var(--border);
    background: var(--surface); color: var(--text); cursor: pointer; font-size: 0.88em;
    transition: border-color 0.15s, background 0.15s; text-decoration: none;
  }
  .btn:hover       { border-color: var(--accent); background: rgba(88,166,255,0.08); }
  .btn-primary     { background: var(--accent); color: #0d1117; border-color: var(--accent); font-weight: 600; }
  .btn-primary:hover { background: #79b8ff; border-color: #79b8ff; }
  .btn-danger      { color: var(--warn); border-color: rgba(248,81,73,0.3); }
  .btn-danger:hover { background: rgba(248,81,73,0.08); border-color: var(--warn); }
  .btn-sm          { padding: 4px 10px; font-size: 0.8em; }
  .form-row        { display: flex; gap: 10px; align-items: flex-end; flex-wrap: wrap; }
  .form-group      { display: flex; flex-direction: column; gap: 5px; flex: 1; min-width: 160px; }
  .form-group label { font-size: 0.8em; color: var(--text-dim); }
  .no-clients      { color: var(--text-dim); font-size: 0.9em; padding: 12px 0; }
  .flash           { padding: 10px 16px; border-radius: 6px; margin-bottom: 20px; font-size: 0.9em; }
  .flash-ok        { background: rgba(63,185,80,0.12);  border: 1px solid rgba(63,185,80,0.3);  color: var(--accent2); }
  .flash-err       { background: rgba(248,81,73,0.12);  border: 1px solid rgba(248,81,73,0.3);  color: var(--warn); }
  .mono            { font-family: "SFMono-Regular", Consolas, monospace; font-size: 0.85em; }
  .fw-name         { color: var(--accent2); }
  .uptime-dim      { color: var(--text-dim); font-size: 0.8em; }
  .drop-zone {
    border: 2px dashed var(--border); border-radius: 6px; padding: 16px 12px;
    text-align: center; color: var(--text-dim); font-size: 0.85em; cursor: pointer;
    transition: border-color 0.15s, background 0.15s; position: relative;
  }
  .drop-zone:hover, .drop-zone.dragover { border-color: var(--accent); background: rgba(88,166,255,0.06); color: var(--text); }
  .drop-zone input[type=file] { position: absolute; inset: 0; opacity: 0; cursor: pointer; width: 100%; height: 100%; }
  .drop-zone .drop-icon { font-size: 1.4em; display: block; margin-bottom: 4px; }
  .drop-zone .drop-filename { display: inline-block; margin-top: 6px; color: var(--accent2); font-weight: 600; font-size: 0.9em; }
  .remove-cell { text-align: right; white-space: nowrap; }
  .btn-remove { color: var(--text-dim); border-color: transparent; font-size: 0.78em; padding: 3px 8px; opacity: 0.55; transition: opacity 0.15s, color 0.15s; }
  .btn-remove:hover { opacity: 1; color: var(--warn); border-color: rgba(248,81,73,0.3); background: rgba(248,81,73,0.06); }
</style>
</head>
<body>
<h1>SyncFrame <span>OTA Admin</span></h1>
<p class="subtitle">Manage remote firmware updates &nbsp;&middot;&nbsp; <a href="{{ home_url }}">&#8592; Back to Photo Viewer</a><br>
<small>Set device <span class="mono" style="color:var(--accent);">cfg.updateUrl</span> to <span class="mono" style="color:var(--accent2);">.../syncframe/ota</span></small></p>

{% if flash_ok %}<div class="flash flash-ok">{{ flash_ok }}</div>{% endif %}
{% if flash_err %}<div class="flash flash-err">{{ flash_err }}</div>{% endif %}

<div class="card">
  <h2>Register Client</h2>
  <form method="POST" action="{{ add_url }}">
    <div class="form-row">
      <div class="form-group"><label>Hostname</label><input type="text" name="hostname" placeholder="syncframe-XXXX" required></div>
      <div class="form-group"><label>MAC Address (optional)</label><input type="text" name="mac" placeholder="AA:BB:CC:DD:EE:FF"></div>
      <div class="form-group"><label>Label / Notes</label><input type="text" name="label" placeholder="Living room frame"></div>
      <div style="display:flex;align-items:flex-end;"><button class="btn btn-primary" type="submit">Add Client</button></div>
    </div>
  </form>
</div>

<div class="card">
  <h2>Registered Clients</h2>
  {% if clients %}
  <table>
    <thead><tr>
      <th>Hostname</th><th>MAC</th><th>Label</th><th>Last Check-In</th>
      <th>Compiled</th><th>Uptime</th><th>Firmware</th><th>Firmware Upload</th><th></th>
    </tr></thead>
    <tbody>
    {% for hostname, info in clients.items() %}
      <tr>
        <td class="mono">{{ hostname }}</td>
        <td class="mono">{{ info.mac if info.mac else '\u2014' }}</td>
        <td>{{ info.label or '\u2014' }}</td>
        <td>
          {% if info.last_seen %}
            <span class="badge {{ 'badge-green' if info.fresh else 'badge-gray' }}">{{ info.last_seen_human }}</span>
          {% else %}<span class="badge badge-gray">Never</span>{% endif %}
        </td>
        <td>
          {% if info.compiled %}<span class="badge badge-yellow mono">{{ info.compiled }}</span>
          {% else %}<span class="badge badge-gray">\u2014</span>{% endif %}
        </td>
        <td>
          {% if info.uptime %}<span class="uptime-dim mono">{{ info.uptime_human }}</span>
          {% else %}<span class="badge badge-gray">\u2014</span>{% endif %}
        </td>
        <td>
          {% if info.fw_compile_id %}
            <span class="badge badge-yellow mono">Pending: {{ info.fw_compile_id }}</span>
          {% elif info.last_flashed %}
            <span class="badge badge-green">\u2713 {{ info.last_flashed_human }}</span>
          {% else %}
            <span class="badge badge-gray">\u2014</span>
          {% endif %}
        </td>
        <td style="min-width:200px">
          <form method="POST" action="{{ upload_fw_url }}" enctype="multipart/form-data" id="fw-form-{{ loop.index }}">
            <input type="hidden" name="hostname" value="{{ hostname }}">
            <div class="drop-zone" id="dz-{{ loop.index }}"
                 ondragover="dzOver(event,'dz-{{ loop.index }}')"
                 ondragleave="dzLeave('dz-{{ loop.index }}')"
                 ondrop="dzDrop(event,'dz-{{ loop.index }}','fw-form-{{ loop.index }}','fn-{{ loop.index }}')"
                 onclick="this.querySelector('input[type=file]').click()">
              <span class="drop-icon">&#128229;</span>Drop .bin or click to browse
              <span class="drop-filename" id="fn-{{ loop.index }}"></span>
              <input type="file" name="firmware" accept=".bin" required
                     onchange="dzPicked(this,'dz-{{ loop.index }}','fn-{{ loop.index }}','fw-form-{{ loop.index }}')">
            </div>
          </form>
          {% if info.fw_compile_id %}
          <form method="POST" action="{{ clear_fw_url }}" style="margin-top:6px;">
            <input type="hidden" name="hostname" value="{{ hostname }}">
            <button class="btn btn-sm btn-danger" type="submit"
                    onclick="return confirm('Clear pending firmware for {{ hostname }}?')">&#10007; Clear pending fw</button>
          </form>
          {% endif %}
        </td>
        <td class="remove-cell">
          <form method="POST" action="{{ remove_url }}">
            <input type="hidden" name="hostname" value="{{ hostname }}">
            <button class="btn btn-sm btn-remove" type="submit"
                    onclick="return confirm('Remove device entry for {{ hostname }}?')">&#128465; Remove device</button>
          </form>
        </td>
      </tr>
    {% endfor %}
    </tbody>
  </table>
  {% else %}
  <p class="no-clients">No clients registered yet.</p>
  {% endif %}
</div>

<div class="card">
  <h2>OTA Endpoint</h2>
  <p style="font-size:0.85em;color:var(--text-dim);">
    Devices GET <span class="mono" style="color:var(--accent2);">{{ ota_url }}</span><br>
    Required headers: <span class="mono" style="color:var(--accent);">X-SF-Hostname</span> &nbsp;
    <span class="mono" style="color:var(--accent);">X-SF-MAC</span> &nbsp;
    <span class="mono" style="color:var(--accent);">X-SF-Compiled</span> &nbsp;
    <span class="mono" style="color:var(--accent);">X-SF-Uptime</span>
  </p>
</div>

<script>
  function dzOver(e,id){e.preventDefault();document.getElementById(id).classList.add('dragover');}
  function dzLeave(id){document.getElementById(id).classList.remove('dragover');}
  function dzDrop(e,dzId,formId,fnId){
    e.preventDefault();dzLeave(dzId);
    const file=e.dataTransfer.files[0];
    if(!file)return;
    if(!file.name.toLowerCase().endsWith('.bin')){alert('Only .bin firmware files are accepted.');return;}
    const form=document.getElementById(formId);
    const input=form.querySelector('input[type=file]');
    const dt=new DataTransfer();dt.items.add(file);input.files=dt.files;
    document.getElementById(fnId).textContent=file.name;
    setTimeout(()=>form.submit(),300);
  }
  function dzPicked(input,dzId,fnId,formId){
    if(!input.files.length)return;
    document.getElementById(fnId).textContent=input.files[0].name;
    setTimeout(()=>document.getElementById(formId).submit(),300);
  }
  setInterval(()=>{location.reload();},60000);
</script>
</body>
</html>
"""


def _humanize(iso_str):
    if not iso_str:
        return None
    try:
        dt = datetime.fromisoformat(iso_str)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        s = int((datetime.now(timezone.utc) - dt).total_seconds())
        if s < 60:
            return f"{s}s ago"
        if s < 3600:
            return f"{s // 60}m ago"
        if s < 86400:
            return f"{s // 3600}h ago"
        return f"{s // 86400}d ago"
    except Exception:
        return iso_str


def _humanize_uptime(seconds_str):
    try:
        s = int(seconds_str)
        days, rem = divmod(s, 86400)
        hours, rem = divmod(rem, 3600)
        minutes = rem // 60
        parts = []
        if days:
            parts.append(f"{days}d")
        if hours:
            parts.append(f"{hours}h")
        parts.append(f"{minutes}m")
        return " ".join(parts)
    except Exception:
        return seconds_str


@bp.route("/admin")
@requires_auth
def admin_page():
    flash_ok = request.args.get("ok")
    flash_err = request.args.get("err")
    with _ota_lock:
        raw_clients = _load_clients()
    clients = {}
    for hostname, info in raw_clients.items():
        entry = dict(info)
        entry["last_seen_human"] = _humanize(info.get("last_seen"))
        entry["last_flashed_human"] = _humanize(info.get("last_flashed"))
        entry["uptime_human"] = (
            _humanize_uptime(info.get("uptime", "")) if info.get("uptime") else None
        )
        try:
            dt = datetime.fromisoformat(info["last_seen"])
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=timezone.utc)
            entry["fresh"] = (datetime.now(timezone.utc) - dt).total_seconds() < 86400
        except Exception:
            entry["fresh"] = False
        clients[hostname] = entry
    ota_url = (URL_PREFIX or "") + "/ota"
    return render_template_string(
        ADMIN_HTML,
        clients=clients,
        flash_ok=flash_ok,
        flash_err=flash_err,
        ota_url=ota_url,
        home_url=url_for("syncframe.index"),
        add_url=url_for("syncframe.admin_add_client"),
        remove_url=url_for("syncframe.admin_remove_client"),
        upload_fw_url=url_for("syncframe.admin_upload_firmware"),
        clear_fw_url=url_for("syncframe.admin_clear_firmware"),
    )


@bp.route("/admin/add", methods=["POST"])
@requires_auth
def admin_add_client():
    hostname = request.form.get("hostname", "").strip()
    mac = request.form.get("mac", "").strip()
    label = request.form.get("label", "").strip()
    if not hostname:
        return redirect(url_for("syncframe.admin_page") + "?err=Hostname+required")
    with _ota_lock:
        clients = _load_clients()
        if hostname in clients:
            return redirect(
                url_for("syncframe.admin_page") + "?err=Client+already+exists"
            )
        clients[hostname] = {
            "hostname": hostname,
            "mac": mac,
            "label": label or hostname,
            "last_seen": None,
            "firmware": None,
            "fw_compile_id": None,
            "compiled": None,
            "uptime": None,
            "last_flashed": None,
        }
        _save_clients(clients)
    return redirect(url_for("syncframe.admin_page") + f"?ok=Client+{hostname}+added")


@bp.route("/admin/remove", methods=["POST"])
@requires_auth
def admin_remove_client():
    hostname = request.form.get("hostname", "").strip()
    if not hostname:
        return redirect(url_for("syncframe.admin_page") + "?err=Hostname+required")
    with _ota_lock:
        clients = _load_clients()
        if hostname in clients:
            fw = clients[hostname].get("firmware")
            del clients[hostname]
            _save_clients(clients)
            if fw and not any(c.get("firmware") == fw for c in clients.values()):
                try:
                    os.remove(os.path.join(OTA_FIRMWARE_DIR, fw))
                except Exception:
                    pass
    return redirect(url_for("syncframe.admin_page") + f"?ok=Client+{hostname}+removed")


@bp.route("/admin/upload_firmware", methods=["POST"])
@requires_auth
def admin_upload_firmware():
    hostname = request.form.get("hostname", "").strip()
    if not hostname:
        return redirect(url_for("syncframe.admin_page") + "?err=Hostname+required")
    if "firmware" not in request.files:
        return redirect(url_for("syncframe.admin_page") + "?err=No+file+provided")
    fw_file = request.files["firmware"]
    if not fw_file or not allowed_firmware(fw_file.filename):
        return redirect(
            url_for("syncframe.admin_page") + "?err=Only+.bin+files+allowed"
        )

    fw_data = fw_file.read()
    compile_id, extract_err = extract_compile_id(fw_data)
    if compile_id is None:
        logging.error("extract_compile_id failed for %s: %s", fw_file.filename, extract_err)
        return (
            f"Could not extract compile ID from firmware.\nDebug info: {extract_err}",
            400,
        )

    safe_hostname = hostname.replace("/", "_").replace("..", "_")
    filename = f"{safe_hostname}--{compile_id}.bin"
    dest = os.path.join(OTA_FIRMWARE_DIR, filename)
    with open(dest, "wb") as f:
        f.write(fw_data)
    logging.info(
        "Firmware saved for %s -> %s (compile_id=%s)", hostname, dest, compile_id
    )

    with _ota_lock:
        clients = _load_clients()
        if hostname not in clients:
            clients[hostname] = {
                "hostname": hostname,
                "mac": "",
                "label": hostname,
                "last_seen": None,
                "compiled": None,
                "uptime": None,
                "last_flashed": None,
            }
        clients[hostname]["firmware"] = filename
        clients[hostname]["fw_compile_id"] = compile_id
        clients[hostname].pop("fw_token", None)
        _save_clients(clients)
    return redirect(
        url_for("syncframe.admin_page")
        + f"?ok=Firmware+uploaded+for+{hostname}+({compile_id})"
    )


@bp.route("/admin/clear_firmware", methods=["POST"])
@requires_auth
def admin_clear_firmware():
    hostname = request.form.get("hostname", "").strip()
    if not hostname:
        return redirect(url_for("syncframe.admin_page") + "?err=Hostname+required")
    with _ota_lock:
        clients = _load_clients()
        if hostname in clients:
            clients[hostname]["firmware"] = None
            clients[hostname]["fw_compile_id"] = None
            _save_clients(clients)
    return redirect(
        url_for("syncframe.admin_page") + f"?ok=Firmware+cleared+for+{hostname}"
    )


# ---------------------------------------------------------------------------
# Register blueprint
# ---------------------------------------------------------------------------
if URL_PREFIX:
    app.register_blueprint(bp, url_prefix=URL_PREFIX)
else:
    app.register_blueprint(bp)


# ---------------------------------------------------------------------------
# Background startup — runs once whether launched via gunicorn or directly.
# Starts mosquitto, the file watcher, and the desaturation scheduler.
# ---------------------------------------------------------------------------
_startup_done = threading.Event()


def _do_startup():
    if _startup_done.is_set():
        return
    _startup_done.set()

    write_mosquitto_conf()
    generate_mqtt_certificates()
    create_mqtt_password_file()

    mosq_conf = os.path.join(MOSQ_DIR, "mosquitto.conf")
    logging.info("Starting Mosquitto MQTT broker...")
    broker_process = None
    try:
        broker_process = subprocess.Popen(
            ["/usr/sbin/mosquitto", "-c", mosq_conf],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError:
        logging.warning(
            "mosquitto binary not found at /usr/sbin/mosquitto; skipping broker start."
        )

    logging.info("Watcher started. Monitoring file changes on %s...", WATCH_FILE)
    logging.info('URL prefix is: "%s"', URL_PREFIX or "/")
    logging.info("Basic auth enabled. Username: %s", USERNAME)

    desaturation_thread = threading.Thread(target=schedule_desaturation, daemon=True)
    desaturation_thread.start()

    watcher_thread = threading.Thread(target=Watcher().run, daemon=True)
    watcher_thread.start()


# Kick off startup when the module is imported (covers gunicorn worker init)
_do_startup()


# ---------------------------------------------------------------------------
# Direct invocation fallback (python3 syncframe-server.py)
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import sys

    if "--launch" in sys.argv:
        # Generate certs if needed, then hand off to gunicorn
        if USE_HTTPS:
            try:
                ensure_certificates(CERTFILE, KEYFILE)
            except Exception as e:
                logging.error("Certificate generation failed: %s", e)
                sys.exit(1)

        gunicorn_args = [
            "gunicorn",
            "--worker-class", "gevent",
            "--workers", "1",
            "--bind", f"{SERVER_HOST}:{SERVER_PORT}",
            "--timeout", "0",
        ]
        if USE_HTTPS:
            gunicorn_args += ["--keyfile", KEYFILE, "--certfile", CERTFILE]
        gunicorn_args.append("syncframe-server:app")

        import shutil
        gunicorn_bin = shutil.which("gunicorn") or "/usr/local/bin/gunicorn"
        logging.info("Handing off to gunicorn: %s", " ".join(gunicorn_args))
        os.execv(gunicorn_bin, gunicorn_args)
        # os.execv replaces this process entirely — nothing below runs
    else:
        # Direct dev-server fallback
        from gevent import monkey
        monkey.patch_all()
        app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)