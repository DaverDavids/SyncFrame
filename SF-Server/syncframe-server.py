import configparser
import hashlib
import logging
import os
import shutil
import ssl
import subprocess
import threading
import time
from datetime import timedelta
from functools import wraps
from io import BytesIO

import paho.mqtt.client as mqtt
import pillow_heif
import schedule
from flask import (Blueprint, Flask, Response, redirect,
                   render_template_string, request, send_from_directory,
                   session, url_for)
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
    # create default config file
    config.read_dict(default_config)
    with open(CONFIG_PATH, "w") as cfgfile:
        config.write(cfgfile)
    logging.info(f"Configuration file created at {CONFIG_PATH} with defaults.")
else:
    config.read(CONFIG_PATH)
    # ensure missing sections/keys are filled with defaults
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

# URL prefix handling (for reverse tunnel at e.g. https://host/syncframe/)
_raw_prefix = config.get(
    "server", "url_prefix", fallback=default_config["server"]["url_prefix"]
).strip()
if _raw_prefix in ("", "/"):
    URL_PREFIX = ""
else:
    # Ensure leading slash, no trailing slash: '/syncframe'
    URL_PREFIX = "/" + _raw_prefix.strip("/")

# Basic auth credentials
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

# Flask web app setup
app = Flask(__name__)
# Set a secret key for session management (change this to a random value in production)
app.secret_key = os.environ.get(
    "SECRET_KEY", "syncframe-secret-key-change-me-in-production"
)
# We'll use a blueprint so url_prefix is applied cleanly
bp = Blueprint("syncframe", __name__)
ALLOWED_EXTENSIONS = {"jpg", "jpeg", "png", "heic", "webp", "gif"}
UPLOAD_FOLDER = os.getcwd()  # Use current directory for uploads


def check_auth(username, password):
    return username == USERNAME and password == PASSWORD


def create_mqtt_password_file():
    """Create MQTT password file."""
    mosq_dir = os.path.join(os.getcwd(), "mosq")
    pwfile = os.path.join(mosq_dir, "pwfile")

    # Check if file doesn't exist OR is empty
    file_is_empty = os.path.exists(pwfile) and os.path.getsize(pwfile) == 0

    if (
        (not os.path.exists(pwfile) or file_is_empty)
        and MQTT_USERNAME
        and MQTT_PASSWORD
    ):
        try:
            # Use mosquitto_passwd to create hashed password
            subprocess.check_call(
                ["mosquitto_passwd", "-c", "-b", pwfile, MQTT_USERNAME, MQTT_PASSWORD],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            os.chmod(pwfile, 0o600)

            # Change ownership to mosquitto user
            try:
                import pwd

                mosquitto_uid = pwd.getpwnam("mosquitto").pw_uid
                mosquitto_gid = pwd.getpwnam("mosquitto").pw_gid
                os.chown(pwfile, mosquitto_uid, mosquitto_gid)
                logging.info(f"Created MQTT password file for user {MQTT_USERNAME}")
            except Exception as e:
                logging.warning(f"Could not change pwfile ownership: {e}")
                # Try more permissive permissions as fallback
                os.chmod(pwfile, 0o644)
                logging.info(
                    f"Created MQTT password file for user {MQTT_USERNAME} (with relaxed permissions)"
                )

        except Exception as e:
            logging.error(f"Failed to create password file: {e}")


def requires_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        # Check if user is already logged in via session
        if session.get("authenticated"):
            return f(*args, **kwargs)

        # Check for Basic Auth credentials
        auth = request.authorization
        if auth and check_auth(auth.username, auth.password):
            # Set session to remember authentication permanently
            session.permanent = True
            session["authenticated"] = True
            # Set session to last for 10 years (effectively never expires)
            app.permanent_session_lifetime = timedelta(days=3650)
            return f(*args, **kwargs)

        # Not authenticated - request Basic Auth
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

    # write key
    with open(keyfile, "wb") as f:
        f.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    os.chmod(keyfile, 0o600)

    # write cert
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

    # create parent directories if necessary
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

        # exif is already a dict: {tag_id: value}
        orientation = exif.get(274)  # 274 = Orientation tag

        if orientation == 3:
            image = image.rotate(180, expand=True)
        elif orientation == 6:
            image = image.rotate(270, expand=True)  # 90° CW
        elif orientation == 8:
            image = image.rotate(90, expand=True)  # 90° CCW

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

    # Check if certificates already exist
    if (
        os.path.exists(ca_crt)
        and os.path.exists(server_crt)
        and os.path.exists(server_key)
    ):
        logging.info("MQTT certificates already exist in ./mosq/")
        return

    logging.info("Generating MQTT certificates in ./mosq/...")

    try:
        # Generate CA key and certificate
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

        # Generate server key
        subprocess.check_call(
            ["openssl", "genrsa", "-out", server_key, "2048"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Generate server certificate signing request
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

        # Sign server certificate with CA
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

        # Set appropriate permissions
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

        # Clean up CSR
        if os.path.exists(server_csr):
            os.remove(server_csr)

        logging.info("MQTT certificates generated successfully in ./mosq/")

    except Exception as e:
        logging.error(f"Failed to generate MQTT certificates: {e}")
        raise


def generate_large_thumbnail():
    """Generate 800x480 thumbnail from main photo"""
    try:
        if not os.path.exists(WATCH_FILE):
            logging.warning(
                "Cannot generate large thumbnail - photo.jpg does not exist"
            )
            return

        img = Image.open(WATCH_FILE)
        img = fix_image_orientation(img)
        large_path = WATCH_FILE.replace("photo.jpg", "photo.800x480.jpg")
        large_img = img.copy()
        large_maxsize = (800, 480)
        large_img.thumbnail(large_maxsize, Image.LANCZOS)
        large_img.save(large_path, format="JPEG", quality=75, optimize=True)
        logging.info(
            f"Large thumbnail regenerated at {large_path} - size: {large_img.size}"
        )
    except Exception as e:
        logging.error(f"Error generating large thumbnail: {e}")


# Class to watch for changes in a specific file
class Watcher:
    FILE_TO_WATCH = WATCH_FILE

    def __init__(self):
        self.observer = Observer()
        self.last_md5 = self.calculate_md5()
        self.last_notification = 0
        self.debounce_ms = 1000  # Wait 1 second after last modification before sending MQTT

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
        # normalize path comparison
        try:
            event_path = os.path.abspath(event.src_path)
            watched_path = os.path.abspath(self.watcher.FILE_TO_WATCH)
        except Exception:
            return None

        if event.is_directory or event_path != watched_path:
            return None

        current_time = int(time.time() * 1000)  # milliseconds
        if current_time - self.watcher.last_notification < self.watcher.debounce_ms:
            # Too soon since last notification, ignore this event
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

    # Configure TLS with self-signed certificates
    ca_crt = os.path.join(os.getcwd(), "mosq", "ca.crt")

    if os.path.exists(ca_crt):
        try:
            client.tls_set(ca_certs=ca_crt, tls_version=ssl.PROTOCOL_TLSv1_2)
            # Skip hostname verification for self-signed certs
            client.tls_insecure_set(True)
            logging.info("MQTT TLS enabled")
        except Exception as e:
            logging.warning(f"Could not enable MQTT TLS: {e}")

    # Set username and password if configured
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
        finalize_changes()
    except Exception as e:
        logging.error(f"Error desaturating image: {e}")


def finalize_changes():
    try:
        shutil.copy("photo-changed.jpg", WATCH_FILE)
        logging.info("Copied photo-changed.jpg to %s", WATCH_FILE)
    except Exception as e:
        logging.error(f"Error copying file: {e}")


def schedule_desaturation():
    schedule.every().day.at("03:00").do(desaturate_image)
    while True:
        schedule.run_pending()
        time.sleep(1)


# Flask routes (on blueprint)
@bp.route("/", methods=["GET"])
@requires_auth
def index():
    last_updated = None
    if os.path.exists(WATCH_FILE):
        last_updated = int(os.path.getmtime(WATCH_FILE) * 1000)
    # use url_for inside template so generated URLs include the prefix automatically
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
            // Update the uploaded photo preview after file selection
            const fileInput = document.querySelector('input[type="file"]');
            const uploadButton = document.querySelector('button[type="submit"]');
            const uploadedPhoto = document.getElementById('uploaded-photo');

            fileInput.addEventListener('change', function(event) {
                const file = event.target.files[0];

                if (file) {
                    // Show preview
                    const reader = new FileReader();
                    reader.onload = function(e) {
                        uploadedPhoto.src = e.target.result;
                        uploadedPhoto.style.display = 'block';
                    };
                    reader.readAsDataURL(file);

                    // Make upload button more prominent
                    uploadButton.style.background = 'rgba(0, 255, 100, 0.8)';
                    uploadButton.style.border = '2px solid rgba(0, 255, 100, 1)';
                    uploadButton.style.transform = 'scale(1.1)';
                    uploadButton.style.boxShadow = '0 0 20px rgba(0, 255, 100, 0.6)';
                    uploadButton.textContent = '✓ Click to Upload!';
                } else {
                    // Reset if no file
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
        </footer>
        <script>
            // Display last updated time in user's local timezone
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

    # Always write to WATCH_FILE (e.g. ./photo.jpg)
    target_path = WATCH_FILE
    ext = file.filename.rsplit(".", 1)[1].lower()

    try:
        data = file.read()
        buf = BytesIO(data)

        # Normalize all uploads into a Pillow Image
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
            # Take first frame of GIF
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
            # Common image types (jpg, jpeg, png, etc.)
            try:
                buf.seek(0)
                img = Image.open(buf)
                img = fix_image_orientation(img)
            except Exception as e:
                logging.error("Image open failed: %s", e)
                return "Image open failed", 500

        # Ensure color mode is RGB for JPEG
        if img.mode not in ("RGB", "L"):
            img = img.convert("RGB")

        # Resize to fit within 1920x1080 while keeping aspect ratio
        max_size = (IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT)
        img.thumbnail(max_size, Image.LANCZOS)

        # Generate large thumbnail RIGHT HERE from img we already have
        large_path = target_path.replace("photo.jpg", "photo.800x480.jpg")
        large_img = img.copy()  # No need to reopen file!
        large_img.thumbnail((800, 480), Image.LANCZOS)
        large_img.save(large_path, format="JPEG", quality=75, optimize=True)
        logging.info(
            "Large thumbnail saved to %s at size %s", large_path, large_img.size
        )

        # Save optimized JPEG
        img.save(target_path, format="JPEG", quality=IMAGE_JPEG_QUALITY, optimize=True)
        logging.info(
            "Uploaded image resized to max %s and saved to %s", max_size, target_path
        )

        # Notify clients (don't send MQTT here - let the file watcher handle it)
        # send_mqtt_message("refresh")  # Commented out to avoid duplicate messages

        return redirect(url_for("syncframe.index"))

    except Exception as e:
        logging.error("Upload failed: %s", e)
        return "Upload failed", 500


@bp.route("/photo.jpg")
@requires_auth
def serve_photo():
    # Serve the watched photo path (the current frame image)
    if os.path.exists(WATCH_FILE):
        return send_from_directory(
            os.path.dirname(os.path.abspath(WATCH_FILE)) or ".",
            os.path.basename(WATCH_FILE),
        )
    return "No photo available", 404


@bp.route("/photo.800x480.jpg")
@requires_auth
def serve_photo_large():
    """Serve the large thumbnail (800x480)"""
    large_file = WATCH_FILE.replace("photo.jpg", "photo.800x480.jpg")
    if os.path.exists(large_file):
        return send_from_directory(
            os.path.dirname(os.path.abspath(large_file)) or ".",
            os.path.basename(large_file),
        )

    # If large thumbnail doesn't exist, try to generate it on-the-fly
    if os.path.exists(WATCH_FILE):
        logging.info("Large thumbnail missing - generating on-the-fly")
        generate_large_thumbnail()
        if os.path.exists(large_file):
            return send_from_directory(
                os.path.dirname(os.path.abspath(large_file)) or ".",
                os.path.basename(large_file),
            )

    return "No photo available - upload an image first", 404


@bp.route("/static/<path:filename>")
def serve_static(filename):
    """Serve static files like favicon.png and manifest - NO AUTH required"""
    return send_from_directory("static", filename)


def allowed_file(filename):
    return "." in filename and filename.rsplit(".", 1)[1].lower() in ALLOWED_EXTENSIONS


# Register blueprint with or without prefix
if URL_PREFIX:
    app.register_blueprint(bp, url_prefix=URL_PREFIX)
else:
    app.register_blueprint(bp)


# Start the Flask web server in the background
def start_web_server():
    if USE_HTTPS:
        try:
            # Ensure certificates exist (create if necessary)
            ensure_certificates(CERTFILE, KEYFILE)
        except Exception as e:
            logging.error("Could not ensure certificates: %s. Falling back to HTTP.", e)
            logging.info(
                "Starting HTTP Flask server on %s:%s", SERVER_HOST, SERVER_PORT
            )
            app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)
            return

        if os.path.exists(CERTFILE) and os.path.exists(KEYFILE):
            logging.info(
                "Starting HTTPS Flask server on %s:%s (prefix=%s)",
                SERVER_HOST,
                SERVER_PORT,
                URL_PREFIX or "/",
            )
            # ssl_context expects either ('cert.pem', 'key.pem') or SSLContext
            app.run(
                host=SERVER_HOST,
                port=SERVER_PORT,
                threaded=True,
                ssl_context=(CERTFILE, KEYFILE),
            )
        else:
            logging.error(
                "CERTFILE or KEYFILE not found after generation. Falling back to HTTP. Cert: %s Key: %s",
                CERTFILE,
                KEYFILE,
            )
            logging.info(
                "Starting HTTP Flask server on %s:%s", SERVER_HOST, SERVER_PORT
            )
            app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)
    else:
        logging.info(
            "Starting HTTP Flask server on %s:%s (prefix=%s)",
            SERVER_HOST,
            SERVER_PORT,
            URL_PREFIX or "/",
        )
        app.run(host=SERVER_HOST, port=SERVER_PORT, threaded=True)


if __name__ == "__main__":
    # Generate MQTT certificates if they don't exist
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
        # Start the watcher
        w = Watcher()
        logging.info("Watcher started. Monitoring file changes on %s...", WATCH_FILE)
        logging.info(
            'URL prefix is: "%s" (use / or empty in config to serve at root)',
            URL_PREFIX or "/",
        )
        logging.info("Basic auth enabled. Username: %s", USERNAME)

        # Start the Flask web server in a separate thread
        web_server_thread = threading.Thread(target=start_web_server)
        web_server_thread.daemon = True
        web_server_thread.start()

        # Start background thread for image desaturation
        desaturation_thread = threading.Thread(target=schedule_desaturation)
        desaturation_thread.daemon = True
        desaturation_thread.start()

        # Run the watcher (blocking)
        w.run()
    except Exception as e:
        logging.error(f"Error: {e}")
    finally:
        # Terminating the Mosquitto MQTT broker
        logging.info("Terminating Mosquitto MQTT broker...")
        if broker_process:
            try:
                broker_process.terminate()
                stdout, stderr = broker_process.communicate(timeout=5)
                logging.info("Broker stdout: %s", stdout.decode())
                logging.info("Broker stderr: %s", stderr.decode())
            except Exception as e:
                logging.error("Error terminating broker: %s", e)
