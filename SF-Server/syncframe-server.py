import configparser
import hashlib
import json
import logging
import os
import shutil
import ssl
import subprocess
import threading
import time
from datetime import datetime, timedelta, timezone
from functools import wraps
from io import BytesIO

import paho.mqtt.client as mqtt
import pillow_heif
import schedule
from flask import (Blueprint, Flask, Response, redirect,
                   render_template_string, request, send_from_directory,
                   session, url_for, jsonify)
from PIL import Image, ImageEnhance, ImageSequence
from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

# Try to import cryptography for certificate generation; if not available, we'll fall back to openssl CLI.
try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    CRYPTOGRAPHY_AVAILABLE = True
except Exception:
    CRYPTOGRAPHY_AVAILABLE = False

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(message)s")

# Load or create configuration
CONFIG_PATH = os.path.join(os.getcwd(), "syncframe-server.conf")
config = configparser.ConfigParser()
default_config = {
    "server": {
        "host": "0.0.0.0",
        "port": "9369",
        # default to true as requested
        "use_https": "true",
        "certfile": "./cert.pem",
        "keyfile": "./key.pem",
        # default prefix where the app is exposed (reverse tunnel subdirectory)
        "url_prefix": "/syncframe",
        "username": "admin",
        "password": "changeme",
    },
    "mqtt": {
        "host": "127.0.0.1",
        "port": "9368",
        "topic": "photos",
        "username": "mqttuser",
        "password": "mqttpass",
    },
    "watcher": {
        "file_to_watch": "./photo.jpg",
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

# Read configuration values
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

_raw_prefix = config.get(
    "server", "url_prefix", fallback=default_config["server"]["url_prefix"]
).strip()
if _raw_prefix in ("", "/"):
    URL_PREFIX = ""
else:
    URL_PREFIX = "/" + _raw_prefix.strip("/")

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

RESOLUTIONS = [
    (800, 480),
    (280, 240),
]

# ---------------------------------------------------------------------------
# OTA / Admin state
# Stored in ota_clients.json alongside the server config.
# Schema: { "hostname": { "mac": str, "label": str, "last_seen": iso8601|null,
#                         "firmware": filename|null } }
# ---------------------------------------------------------------------------
OTA_CLIENTS_PATH = os.path.join(os.getcwd(), "ota_clients.json")
OTA_FIRMWARE_DIR = os.path.join(os.getcwd(), "ota_firmware")
os.makedirs(OTA_FIRMWARE_DIR, exist_ok=True)

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


def _rebuild_manifest():
    """Write ota_firmware/manifest.txt based on current client->firmware mappings."""
    clients = _load_clients()
    lines = []
    for hostname, info in clients.items():
        fw = info.get("firmware")
        if fw:
            lines.append(f"{hostname} {fw}")
    manifest_path = os.path.join(OTA_FIRMWARE_DIR, "manifest.txt")
    with open(manifest_path, "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))
    logging.info("manifest.txt rebuilt with %d entries", len(lines))


# Flask web app setup
app = Flask(__name__)
app.secret_key = os.environ.get(
    "SECRET_KEY", "syncframe-secret-key-change-me-in-production"
)
bp = Blueprint("syncframe", __name__)
ALLOWED_EXTENSIONS = {"jpg", "jpeg", "png", "heic", "webp", "gif"}
ALLOWED_FIRMWARE_EXTENSIONS = {"bin"}
UPLOAD_FOLDER = os.getcwd()


def check_auth(username, password):
    return username == USERNAME and password == PASSWORD


def create_mqtt_password_file():
    """Create MQTT password file."""
    mosq_dir = os.path.join(os.getcwd(), "mosq")
    pwfile = os.path.join(mosq_dir, "pwfile")

    file_is_empty = os.path.exists(pwfile) and os.path.getsize(pwfile) == 0

    if (
        (not os.path.exists(pwfile) or file_is_empty)
        and MQTT_USERNAME
        and MQTT_PASSWORD
    ):
        try:
            subprocess.check_call(
                ["mosquitto_passwd", "-c", "-b", pwfile, MQTT_USERNAME, MQTT_PASSWORD],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            os.chmod(pwfile, 0o600)

            try:
                import pwd

                mosquitto_uid = pwd.getpwnam("mosquitto").pw_uid
                mosquitto_gid = pwd.getpwnam("mosquitto").pw_gid
                os.chown(pwfile, mosquitto_uid, mosquitto_gid)
                logging.info(f"Created MQTT password file for user {MQTT_USERNAME}")
            except Exception as e:
                logging.warning(f"Could not change pwfile ownership: {e}")
                os.chmod(pwfile, 0o644)
                logging.info(
                    f"Created MQTT password file for user {MQTT_USERNAME} (with relaxed permissions)"
                )

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
    """Generate a self-signed cert using cryptography library."""
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
        .not_valid_before(
            x509.datetime.datetime.utcnow() - x509.datetime.timedelta(days=1)
        )
        .not_valid_after(
            x509.datetime.datetime.utcnow() + x509.datetime.timedelta(days=days_valid)
        )
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
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
    """Generate a self-signed cert using openssl CLI as fallback."""
    subj = f"/CN={common_name}"
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
        subj,
    ]
    try:
        subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(keyfile, 0o600)
    except Exception as e:
        logging.error("OpenSSL certificate generation failed: %s", e)
        raise


def ensure_certificates(certfile, keyfile):
    """Ensure both certificate and key exist. If not, create them."""
    cert_exists = os.path.exists(certfile)
    key_exists = os.path.exists(keyfile)

    if cert_exists and key_exists:
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
    """Fix image orientation based on EXIF data."""
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
    """Generate CA and server certificates for MQTT broker in ./mosq/ directory."""
    mosq_dir = os.path.join(os.getcwd(), "mosq")
    os.makedirs(mosq_dir, exist_ok=True)

    ca_key = os.path.join(mosq_dir, "ca.key")
    ca_crt = os.path.join(mosq_dir, "ca.crt")
    server_key = os.path.join(mosq_dir, "server.key")
    server_crt = os.path.join(mosq_dir, "server.crt")
    server_csr = os.path.join(mosq_dir, "server.csr")

    if (
        os.path.exists(ca_crt)
        and os.path.exists(server_crt)
        and os.path.exists(server_key)
    ):
        logging.info("MQTT certificates already exist in ./mosq/")
        return

    logging.info("Generating MQTT certificates in ./mosq/...")

    try:
        subprocess.check_call(
            ["openssl", "genrsa", "-out", ca_key, "2048"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        subprocess.check_call(
            [
                "openssl", "req", "-new", "-x509", "-days", "3650",
                "-key", ca_key, "-out", ca_crt, "-subj", "/CN=MQTT-CA",
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
                "openssl", "req", "-new", "-key", server_key,
                "-out", server_csr, "-subj", "/CN=localhost",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        subprocess.check_call(
            [
                "openssl", "x509", "-req", "-in", server_csr,
                "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
                "-out", server_crt, "-days", "3650",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        os.chmod(ca_key, 0o600)
        os.chmod(server_key, 0o600)

        try:
            import pwd

            mosquitto_uid = pwd.getpwnam("mosquitto").pw_uid
            mosquitto_gid = pwd.getpwnam("mosquitto").pw_gid

            os.chown(ca_crt, mosquitto_uid, mosquitto_gid)
            os.chown(server_crt, mosquitto_uid, mosquitto_gid)
            os.chown(server_key, mosquitto_uid, mosquitto_gid)
        except Exception as e:
            logging.warning(f"Could not change certificate ownership: {e}")

        if os.path.exists(server_csr):
            os.remove(server_csr)

        logging.info("MQTT certificates generated successfully in ./mosq/")

    except Exception as e:
        logging.error(f"Failed to generate MQTT certificates: {e}")
        raise


def generate_thumbnails(source_img=None):
    """Generate all resolution variants listed in RESOLUTIONS from the master photo.jpg."""
    try:
        if source_img is None:
            if not os.path.exists(WATCH_FILE):
                logging.warning("Cannot generate thumbnails - photo.jpg does not exist")
                return
            source_img = Image.open(WATCH_FILE)
            source_img = fix_image_orientation(source_img)

        for (w, h) in RESOLUTIONS:
            try:
                thumb = source_img.copy()
                thumb.thumbnail((w, h), Image.LANCZOS)
                out_path = WATCH_FILE.replace("photo.jpg", f"photo.{w}x{h}.jpg")
                thumb.save(out_path, format="JPEG", quality=75, optimize=True)
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


def send_mqtt_message(message):
    logging.info("Sending MQTT message: %s", message)
    client = mqtt.Client()

    ca_crt = os.path.join(os.getcwd(), "mosq", "ca.crt")

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
        img.save("photo-changed.jpg")
        logging.info(
            "Desaturated %s by 34%% and saved to photo-changed.jpg", WATCH_FILE
        )
        finalize_changes(img)
    except Exception as e:
        logging.error(f"Error desaturating image: {e}")


def finalize_changes(img=None):
    try:
        shutil.copy("photo-changed.jpg", WATCH_FILE)
        logging.info("Copied photo-changed.jpg to %s", WATCH_FILE)
        generate_thumbnails(source_img=img)
    except Exception as e:
        logging.error(f"Error in finalize_changes: {e}")


def schedule_desaturation():
    schedule.every().day.at("03:00").do(desaturate_image)
    while True:
        schedule.run_pending()
        time.sleep(1)


# ---------------------------------------------------------------------------
# Flask routes - main photo viewer
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

        h1 {
            color: #ffffff;
            text-shadow: 0 2px 10px rgba(0, 0, 0, 0.7);
            font-size: 2.5em;
            margin-bottom: 30px;
        }

        form {
            margin-bottom: 30px;
        }

        .container {
            display: flex;
            flex-wrap: wrap;
            justify-content: space-around;
            align-items: center;
            gap: 60px;
        }

        .image-container {
            flex: 1;
            min-width: 300px;
            max-width: 45%;
            text-align: center;
            background: #000000;
            padding: 0;
            margin-top: 50px;
            border-radius: 4px;
            border: 15px solid;
            border-image: linear-gradient(145deg, #8b6f47, #d4a574, #c4956a, #8b6f47, #6a5537) 1;
            box-shadow:
                inset 0 0 30px rgba(0, 0, 0, 0.8),
                0 10px 40px rgba(0, 0, 0, 0.8),
                0 0 0 3px #4a3a2a,
                0 0 0 4px #2a1a0a;
            position: relative;
            aspect-ratio: 16 / 9;
            overflow: visible;
        }

        .image-container img {
            width: 100%;
            height: 100%;
            object-fit: contain;
            border: none;
            border-radius: 2px;
            position: relative;
            z-index: 1;
            display: block;
        }

        .image-container h2 {
            position: absolute;
            top: -60px;
            left: -40px;
            right: -40px;
            bottom: -40px;
            margin: 0;
            padding: 15px 20px;
            padding-bottom: 60px;
            font-size: 1.3em;
            color: #ffffff;
            text-shadow: 0 2px 8px rgba(0, 0, 0, 0.6);
            background: rgba(0, 0, 0, 0.4);
            backdrop-filter: blur(10px);
            border-radius: 12px;
            z-index: -1;
            display: flex;
            align-items: flex-start;
            justify-content: center;
        }

        input[type="file"] {
            margin-bottom: 20px;
            background: rgba(255, 255, 255, 0.15);
            color: #ffffff;
            border: 1px solid rgba(255, 255, 255, 0.3);
            padding: 10px 20px;
            border-radius: 6px;
            backdrop-filter: blur(10px);
            cursor: pointer;
        }

        input[type="file"]::file-selector-button {
            background: rgba(255, 255, 255, 0.2);
            color: #ffffff;
            border: none;
            padding: 5px 15px;
            border-radius: 4px;
            cursor: pointer;
            margin-right: 10px;
        }

        input[type="file"]::file-selector-button:hover {
            background: rgba(255, 255, 255, 0.3);
        }

        button {
            background: rgba(255, 255, 255, 0.15);
            color: #ffffff;
            border: 1px solid rgba(255, 255, 255, 0.3);
            padding: 10px 20px;
            border-radius: 6px;
            cursor: pointer;
            backdrop-filter: blur(10px);
            font-size: 1em;
            transition: background 0.3s ease;
        }

        button:hover {
            background: rgba(255, 255, 255, 0.25);
        }

        .admin-link {
            display: inline-block;
            margin-top: 20px;
            color: rgba(255,255,255,0.6);
            text-decoration: none;
            font-size: 0.85em;
            border: 1px solid rgba(255,255,255,0.2);
            padding: 6px 16px;
            border-radius: 4px;
            transition: all 0.2s;
        }
        .admin-link:hover {
            color: #fff;
            border-color: rgba(255,255,255,0.5);
            background: rgba(255,255,255,0.1);
        }

        @media (max-width: 768px) {
            .container {
                flex-direction: column;
            }
            .image-container {
                max-width: 100%;
            }
            h1 {
                font-size: 2em;
            }
        }

        footer {
            margin-bottom: 0;
        }

        footer p {
            margin-bottom: 0;
        }
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
            const fileInput = document.querySelector('input[type="file"]');
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

                    uploadButton.style.background = 'rgba(0, 255, 100, 0.8)';
                    uploadButton.style.border = '2px solid rgba(0, 255, 100, 1)';
                    uploadButton.style.transform = 'scale(1.1)';
                    uploadButton.style.boxShadow = '0 0 20px rgba(0, 255, 100, 0.6)';
                    uploadButton.textContent = '✓ Click to Upload!';
                } else {
                    uploadedPhoto.src = '';
                    uploadedPhoto.style.display = 'none';
                    uploadButton.style.background = 'rgba(255, 255, 255, 0.15)';
                    uploadButton.style.border = '1px solid rgba(255, 255, 255, 0.3)';
                    uploadButton.style.transform = 'scale(1)';
                    uploadButton.style.boxShadow = 'none';
                    uploadButton.textContent = 'Upload Photo';
                }
            });
        </script>
        <footer style="margin-top: 10px; padding: 5px; color: rgba(255, 255, 255, 0.7); font-size: 0.9em;">
            <p id="last-updated">
                {% if last_updated %}Last updated: <span id='update-time'></span>{% else %}No photo uploaded yet{% endif %}
            </p>
            <a class="admin-link" href="{{ url_for('syncframe.admin_page') }}">⚙ OTA Admin</a>
        </footer>
        <script>
            const lastUpdated = {{ last_updated if last_updated else 'null' }};
            if (lastUpdated) {
                const date = new Date(lastUpdated);
                const options = {
                    month: 'short',
                    day: 'numeric',
                    hour: 'numeric',
                    minute: '2-digit',
                    hour12: true
                };
                document.getElementById('update-time').textContent = date.toLocaleString('en-US', options);
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
                if first_frame.mode in ("RGBA", "P", "LA"):
                    img = first_frame.convert("RGB")
                else:
                    img = first_frame
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

        generate_thumbnails(source_img=img)

        return redirect(url_for("syncframe.index"))

    except Exception as e:
        logging.error("Upload failed: %s", e)
        return "Upload failed", 500


@bp.route("/photo.jpg")
@requires_auth
def serve_photo():
    if os.path.exists(WATCH_FILE):
        return send_from_directory(
            os.path.dirname(os.path.abspath(WATCH_FILE)) or ".",
            os.path.basename(WATCH_FILE),
        )
    return "No photo available", 404


@bp.route("/photo.<int:w>x<int:h>.jpg")
@requires_auth
def serve_photo_variant(w, h):
    if (w, h) not in RESOLUTIONS:
        return "Resolution not supported", 404

    variant_file = WATCH_FILE.replace("photo.jpg", f"photo.{w}x{h}.jpg")

    if os.path.exists(variant_file):
        return send_from_directory(
            os.path.dirname(os.path.abspath(variant_file)) or ".",
            os.path.basename(variant_file),
        )

    if os.path.exists(WATCH_FILE):
        logging.info("Variant %dx%d missing - generating all thumbnails on-the-fly", w, h)
        generate_thumbnails()
        if os.path.exists(variant_file):
            return send_from_directory(
                os.path.dirname(os.path.abspath(variant_file)) or ".",
                os.path.basename(variant_file),
            )

    return "No photo available - upload an image first", 404


@bp.route("/static/<path:filename>")
def serve_static(filename):
    """Serve static files - NO AUTH required"""
    return send_from_directory("static", filename)


def allowed_file(filename):
    return "." in filename and filename.rsplit(".", 1)[1].lower() in ALLOWED_EXTENSIONS


def allowed_firmware(filename):
    return "." in filename and filename.rsplit(".", 1)[1].lower() in ALLOWED_FIRMWARE_EXTENSIONS


# ---------------------------------------------------------------------------
# OTA public endpoints (no auth - ESP32 fetches these)
# ---------------------------------------------------------------------------

@bp.route("/manifest.txt")
def serve_manifest():
    """Serve the OTA manifest. Public - no auth - ESP32 polls this."""
    manifest_path = os.path.join(OTA_FIRMWARE_DIR, "manifest.txt")
    if not os.path.exists(manifest_path):
        # Return empty manifest if none exists yet
        return Response("", mimetype="text/plain")
    return send_from_directory(OTA_FIRMWARE_DIR, "manifest.txt", mimetype="text/plain")


@bp.route("/firmware/<path:filename>")
def serve_firmware(filename):
    """Serve firmware .bin files. Public - no auth - ESP32 downloads these."""
    safe_name = os.path.basename(filename)
    fw_path = os.path.join(OTA_FIRMWARE_DIR, safe_name)
    if not os.path.exists(fw_path):
        return "Firmware not found", 404
    return send_from_directory(OTA_FIRMWARE_DIR, safe_name,
                                mimetype="application/octet-stream")


@bp.route("/checkin", methods=["POST"])
def ota_checkin():
    """
    ESP32 clients POST here to record their presence.
    Expected JSON body: { "hostname": "syncframe-4A2", "mac": "AA:BB:CC:DD:EE:FF" }
    Or form fields with the same keys.
    Returns 200 JSON with { "firmware": "<filename or null>" }
    """
    if request.is_json:
        data = request.get_json(silent=True) or {}
    else:
        data = request.form

    hostname = (data.get("hostname") or "").strip()
    mac = (data.get("mac") or "").strip()

    if not hostname:
        return jsonify({"error": "hostname required"}), 400

    now_iso = datetime.now(timezone.utc).isoformat()

    with _ota_lock:
        clients = _load_clients()
        if hostname not in clients:
            # Auto-register unknown clients so they appear in the admin UI
            clients[hostname] = {"mac": mac, "label": hostname, "last_seen": now_iso, "firmware": None}
            logging.info("OTA: auto-registered new client %s (%s)", hostname, mac)
        else:
            clients[hostname]["last_seen"] = now_iso
            if mac:
                clients[hostname]["mac"] = mac
        firmware = clients[hostname].get("firmware")
        _save_clients(clients)

    return jsonify({"firmware": firmware}), 200


# ---------------------------------------------------------------------------
# OTA Admin page (auth required)
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
    --bg: #0d1117;
    --surface: #161b22;
    --border: #30363d;
    --accent: #58a6ff;
    --accent2: #3fb950;
    --warn: #f85149;
    --text: #c9d1d9;
    --text-dim: #8b949e;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; padding: 24px; }
  h1 { font-size: 1.6em; margin-bottom: 6px; }
  h1 span { color: var(--accent); }
  .subtitle { color: var(--text-dim); font-size: 0.9em; margin-bottom: 28px; }
  .subtitle a { color: var(--accent); text-decoration: none; }
  .card { background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 20px; margin-bottom: 24px; }
  .card h2 { font-size: 1.05em; margin-bottom: 16px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.05em; font-size: 0.8em; }
  table { width: 100%; border-collapse: collapse; font-size: 0.9em; }
  th { text-align: left; padding: 8px 12px; color: var(--text-dim); border-bottom: 1px solid var(--border); font-weight: 500; font-size: 0.8em; text-transform: uppercase; }
  td { padding: 10px 12px; border-bottom: 1px solid var(--border); vertical-align: middle; }
  tr:last-child td { border-bottom: none; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 0.75em; font-weight: 600; }
  .badge-green { background: rgba(63,185,80,0.15); color: var(--accent2); border: 1px solid rgba(63,185,80,0.3); }
  .badge-gray  { background: rgba(139,148,158,0.15); color: var(--text-dim); border: 1px solid rgba(139,148,158,0.2); }
  .badge-blue  { background: rgba(88,166,255,0.15); color: var(--accent); border: 1px solid rgba(88,166,255,0.3); }
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
  .btn:hover { border-color: var(--accent); background: rgba(88,166,255,0.08); }
  .btn-primary { background: var(--accent); color: #0d1117; border-color: var(--accent); font-weight: 600; }
  .btn-primary:hover { background: #79b8ff; border-color: #79b8ff; }
  .btn-danger { color: var(--warn); border-color: rgba(248,81,73,0.3); }
  .btn-danger:hover { background: rgba(248,81,73,0.08); border-color: var(--warn); }
  .btn-sm { padding: 4px 10px; font-size: 0.8em; }
  .form-row { display: flex; gap: 10px; align-items: flex-end; flex-wrap: wrap; }
  .form-group { display: flex; flex-direction: column; gap: 5px; flex: 1; min-width: 160px; }
  .form-group label { font-size: 0.8em; color: var(--text-dim); }
  .no-clients { color: var(--text-dim); font-size: 0.9em; padding: 12px 0; }
  .flash { padding: 10px 16px; border-radius: 6px; margin-bottom: 20px; font-size: 0.9em; }
  .flash-ok  { background: rgba(63,185,80,0.12); border: 1px solid rgba(63,185,80,0.3); color: var(--accent2); }
  .flash-err { background: rgba(248,81,73,0.12); border: 1px solid rgba(248,81,73,0.3); color: var(--warn); }
  .mono { font-family: "SFMono-Regular", Consolas, monospace; font-size: 0.85em; }
  .fw-name { color: var(--accent2); }
  .stale { color: var(--warn); }
  details summary { cursor: pointer; color: var(--text-dim); font-size: 0.85em; margin-top: 10px; }
  details[open] summary { margin-bottom: 10px; }
</style>
</head>
<body>
<h1>SyncFrame <span>OTA Admin</span></h1>
<p class="subtitle">
  Manage remote firmware updates &nbsp;·&nbsp;
  <a href="{{ home_url }}">← Back to Photo Viewer</a>
</p>

{% if flash_ok %}<div class="flash flash-ok">{{ flash_ok }}</div>{% endif %}
{% if flash_err %}<div class="flash flash-err">{{ flash_err }}</div>{% endif %}

<!-- Add client -->
<div class="card">
  <h2>Register Client</h2>
  <form method="POST" action="{{ add_url }}">
    <div class="form-row">
      <div class="form-group">
        <label>Hostname (e.g. syncframe-4A2)</label>
        <input type="text" name="hostname" placeholder="syncframe-XXXX" required>
      </div>
      <div class="form-group">
        <label>MAC Address (optional)</label>
        <input type="text" name="mac" placeholder="AA:BB:CC:DD:EE:FF">
      </div>
      <div class="form-group">
        <label>Label / Notes</label>
        <input type="text" name="label" placeholder="Living room frame">
      </div>
      <div style="display:flex;align-items:flex-end;">
        <button class="btn btn-primary" type="submit">Add Client</button>
      </div>
    </div>
  </form>
</div>

<!-- Client table -->
<div class="card">
  <h2>Registered Clients</h2>
  {% if clients %}
  <table>
    <thead>
      <tr>
        <th>Hostname</th>
        <th>MAC</th>
        <th>Label</th>
        <th>Last Check-In</th>
        <th>Pending Firmware</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>
    {% for hostname, info in clients.items() %}
      <tr>
        <td class="mono">{{ hostname }}</td>
        <td class="mono">{{ info.mac or '—' }}</td>
        <td>{{ info.label or '—' }}</td>
        <td>
          {% if info.last_seen %}
            <span class="badge {{ 'badge-green' if info.fresh else 'badge-gray' }}">
              {{ info.last_seen_human }}
            </span>
          {% else %}
            <span class="badge badge-gray">Never</span>
          {% endif %}
        </td>
        <td>
          {% if info.firmware %}
            <span class="badge badge-blue fw-name mono">{{ info.firmware }}</span>
          {% else %}
            <span class="badge badge-gray">None</span>
          {% endif %}
        </td>
        <td>
          <!-- Upload firmware for this client -->
          <details>
            <summary>Upload firmware</summary>
            <form method="POST" action="{{ upload_fw_url }}" enctype="multipart/form-data"
                  style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:4px;">
              <input type="hidden" name="hostname" value="{{ hostname }}">
              <input type="file" name="firmware" accept=".bin" required style="flex:1;min-width:180px;">
              <button class="btn btn-primary btn-sm" type="submit">Upload .bin</button>
            </form>
          </details>
          {% if info.firmware %}
          &nbsp;
          <form method="POST" action="{{ clear_fw_url }}" style="display:inline;">
            <input type="hidden" name="hostname" value="{{ hostname }}">
            <button class="btn btn-sm btn-danger" type="submit"
                    onclick="return confirm('Clear firmware for {{ hostname }}?')">Clear fw</button>
          </form>
          {% endif %}
          &nbsp;
          <form method="POST" action="{{ remove_url }}" style="display:inline;">
            <input type="hidden" name="hostname" value="{{ hostname }}">
            <button class="btn btn-sm btn-danger" type="submit"
                    onclick="return confirm('Remove {{ hostname }}?')">Remove</button>
          </form>
        </td>
      </tr>
    {% endfor %}
    </tbody>
  </table>
  {% else %}
  <p class="no-clients">No clients registered yet. Add one above or let a device check in automatically.</p>
  {% endif %}
</div>

<!-- Manifest preview -->
<div class="card">
  <h2>Current manifest.txt</h2>
  <pre class="mono" style="white-space:pre-wrap;color:var(--accent2);font-size:0.85em;">{{ manifest_content or '(empty)' }}</pre>
  <p style="margin-top:10px;font-size:0.8em;color:var(--text-dim);">
    Served at <span class="mono">{{ manifest_url }}</span>
  </p>
</div>

<script>
  // Auto-refresh last-seen timestamps
  setInterval(() => { location.reload(); }, 60000);
</script>
</body>
</html>
"""


def _humanize(iso_str):
    """Return a human-readable relative time string from an ISO8601 UTC string."""
    if not iso_str:
        return None
    try:
        dt = datetime.fromisoformat(iso_str)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        delta = datetime.now(timezone.utc) - dt
        s = int(delta.total_seconds())
        if s < 60:
            return f"{s}s ago"
        elif s < 3600:
            return f"{s // 60}m ago"
        elif s < 86400:
            return f"{s // 3600}h ago"
        else:
            return f"{s // 86400}d ago"
    except Exception:
        return iso_str


@bp.route("/admin")
@requires_auth
def admin_page():
    flash_ok = request.args.get("ok")
    flash_err = request.args.get("err")

    with _ota_lock:
        raw_clients = _load_clients()

    # Annotate clients with human-readable / freshness info
    clients = {}
    for hostname, info in raw_clients.items():
        entry = dict(info)
        entry["last_seen_human"] = _humanize(info.get("last_seen"))
        # "fresh" = checked in within the last 24 hours
        try:
            dt = datetime.fromisoformat(info["last_seen"])
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=timezone.utc)
            entry["fresh"] = (datetime.now(timezone.utc) - dt).total_seconds() < 86400
        except Exception:
            entry["fresh"] = False
        clients[hostname] = entry

    manifest_path = os.path.join(OTA_FIRMWARE_DIR, "manifest.txt")
    manifest_content = ""
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest_content = f.read().strip()

    # Build the public manifest URL hint (best-effort)
    manifest_url = (URL_PREFIX or "") + "/manifest.txt"

    return render_template_string(
        ADMIN_HTML,
        clients=clients,
        flash_ok=flash_ok,
        flash_err=flash_err,
        manifest_content=manifest_content,
        manifest_url=manifest_url,
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
            return redirect(url_for("syncframe.admin_page") + "?err=Client+already+exists")
        clients[hostname] = {"mac": mac, "label": label or hostname, "last_seen": None, "firmware": None}
        _save_clients(clients)
        _rebuild_manifest()

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
            # Optionally remove firmware file if no other client uses it
            fw = clients[hostname].get("firmware")
            del clients[hostname]
            _save_clients(clients)
            _rebuild_manifest()
            # Clean up orphaned firmware
            if fw and not any(c.get("firmware") == fw for c in clients.values()):
                fw_path = os.path.join(OTA_FIRMWARE_DIR, fw)
                try:
                    os.remove(fw_path)
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
        return redirect(url_for("syncframe.admin_page") + "?err=Only+.bin+files+allowed")

    # Name the file after the hostname for clarity
    safe_hostname = hostname.replace("/", "_").replace("..", "_")
    filename = f"{safe_hostname}.bin"
    dest = os.path.join(OTA_FIRMWARE_DIR, filename)
    fw_file.save(dest)
    logging.info("Firmware saved for %s -> %s", hostname, dest)

    with _ota_lock:
        clients = _load_clients()
        if hostname not in clients:
            clients[hostname] = {"mac": "", "label": hostname, "last_seen": None, "firmware": None}
        clients[hostname]["firmware"] = filename
        _save_clients(clients)
        _rebuild_manifest()

    return redirect(url_for("syncframe.admin_page") + f"?ok=Firmware+uploaded+for+{hostname}")


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
            _save_clients(clients)
            _rebuild_manifest()

    return redirect(url_for("syncframe.admin_page") + f"?ok=Firmware+cleared+for+{hostname}")


# Register blueprint with or without prefix
if URL_PREFIX:
    app.register_blueprint(bp, url_prefix=URL_PREFIX)
else:
    app.register_blueprint(bp)


def start_web_server():
    if USE_HTTPS:
        try:
            ensure_certificates(CERTFILE, KEYFILE)
        except Exception as e:
            logging.error("Could not ensure certificates: %s. Falling back to HTTP.", e)
            logging.info("Starting HTTP Flask server on %s:%s", SERVER_HOST, SERVER_PORT)
            app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)
            return

        if os.path.exists(CERTFILE) and os.path.exists(KEYFILE):
            logging.info(
                "Starting HTTPS Flask server on %s:%s (prefix=%s)",
                SERVER_HOST, SERVER_PORT, URL_PREFIX or "/",
            )
            app.run(
                host=SERVER_HOST,
                port=SERVER_PORT,
                threaded=True,
                ssl_context=(CERTFILE, KEYFILE),
            )
        else:
            logging.error(
                "CERTFILE or KEYFILE not found after generation. Falling back to HTTP. Cert: %s Key: %s",
                CERTFILE, KEYFILE,
            )
            logging.info("Starting HTTP Flask server on %s:%s", SERVER_HOST, SERVER_PORT)
            app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)
    else:
        logging.info(
            "Starting HTTP Flask server on %s:%s (prefix=%s)",
            SERVER_HOST, SERVER_PORT, URL_PREFIX or "/",
        )
        app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)


if __name__ == "__main__":
    generate_mqtt_certificates()
    create_mqtt_password_file()

    logging.info("Starting Mosquitto MQTT broker...")
    try:
        broker_process = subprocess.Popen(
            ["/usr/sbin/mosquitto", "-c", "./mosquitto.conf", "-p", str(MQTT_PORT)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError:
        broker_process = None
        logging.warning(
            "mosquitto binary not found at /usr/sbin/mosquitto; skipping broker start."
        )

    try:
        w = Watcher()
        logging.info("Watcher started. Monitoring file changes on %s...", WATCH_FILE)
        logging.info(
            'URL prefix is: "%s" (use / or empty in config to serve at root)',
            URL_PREFIX or "/",
        )
        logging.info("Basic auth enabled. Username: %s", USERNAME)

        web_server_thread = threading.Thread(target=start_web_server)
        web_server_thread.daemon = True
        web_server_thread.start()

        desaturation_thread = threading.Thread(target=schedule_desaturation)
        desaturation_thread.daemon = True
        desaturation_thread.start()

        w.run()
    except Exception as e:
        logging.error(f"Error: {e}")
    finally:
        logging.info("Terminating Mosquitto MQTT broker...")
        if broker_process:
            try:
                broker_process.terminate()
                stdout, stderr = broker_process.communicate(timeout=5)
                logging.info("Broker stdout: %s", stdout.decode())
                logging.info("Broker stderr: %s", stderr.decode())
            except Exception as e:
                logging.error("Error terminating broker: %s", e)
