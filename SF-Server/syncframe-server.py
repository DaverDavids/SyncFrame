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


def ensure_certificates(cert_path, key_path):
    if os.path.exists(cert_path) and os.path.exists(key_path):
        return
    if not CRYPTOGRAPHY_AVAILABLE:
        raise RuntimeError(
            "cryptography package is required to generate self-signed certificates"
        )

    os.makedirs(os.path.dirname(cert_path), exist_ok=True)
    os.makedirs(os.path.dirname(key_path), exist_ok=True)

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name(
        [
            x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
            x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, "Ohio"),
            x509.NameAttribute(NameOID.LOCALITY_NAME, "Centerville"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "SyncFrame"),
            x509.NameAttribute(NameOID.COMMON_NAME, "syncframe.local"),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.now(timezone.utc) - timedelta(days=1))
        .not_valid_after(datetime.now(timezone.utc) + timedelta(days=3650))
        .add_extension(
            x509.SubjectAlternativeName(
                [
                    x509.DNSName("localhost"),
                    x509.DNSName("syncframe.local"),
                ]
            ),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )

    with open(key_path, "wb") as f:
        f.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

# ---------------------------------------------------------------------------
# Data directory
# ---------------------------------------------------------------------------
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
    logging.info(
        "Set %s ownership to mosquitto (%d:%d)", MOSQ_DIR, _mosq.pw_uid, _mosq.pw_gid
    )
except KeyError:
    logging.warning("mosquitto user not found - skipping chown of %s", MOSQ_DIR)
except Exception as _e:
    logging.warning("Could not chown %s to mosquitto: %s", MOSQ_DIR, _e)

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for _fname in os.listdir(_SCRIPT_DIR):
    if _fname.lower().endswith(".jpg"):
        _src = os.path.join(_SCRIPT_DIR, _fname)
        _dst = os.path.join(DATA_DIR, _fname)
        if not os.path.exists(_dst):
            shutil.copy2(_src, _dst)
            logging.info("Seeded default image: %s -> %s", _src, _dst)

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

OLD_SECRET_KEY = "syncframe-secret-key-change-me-in-production"

secret_key_value = config.get("server", "secret_key", fallback="").strip()
if not secret_key_value:
    secret_key_value = secrets.token_hex(32)
    config.set("server", "secret_key", secret_key_value)
    with open(CONFIG_PATH, "w") as cfgfile:
        config.write(cfgfile)
    logging.info("Generated new SECRET_KEY and saved to config.")

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
WATCH_FILE = os.path.abspath(config.get("watcher", "file_to_watch", fallback="./data/photo.jpg"))
connected_stream_clients: dict = {}  # keyed by MAC → {"queue": queue.Queue, "resolution": (w,h), "hostname": str, "uptime": int, "compiled": str}
_stream_lock = threading.Lock()


def _compute_crc32(path: str) -> str:
    val = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            val = zlib.crc32(chunk, val)
    return format(val & 0xFFFFFFFF, "08x")


current_photo_hash: str = _compute_crc32(WATCH_FILE) if os.path.exists(WATCH_FILE) else ""


def _variant_path_for_resolution(resolution):
    w, h = resolution
    return os.path.join(os.path.dirname(WATCH_FILE), f"photo.{w}x{h}.jpg")


def _push_photo_to_stream_clients():
    with _stream_lock:
        clients_snapshot = dict(connected_stream_clients)
        photo_hash_snapshot = current_photo_hash

    for mac, client in clients_snapshot.items():
        resolution = client.get("resolution", (800, 480))
        variant_file = _variant_path_for_resolution(resolution)
        if not os.path.exists(variant_file):
            variant_file = WATCH_FILE
        try:
            with open(variant_file, "rb") as f:
                jpeg_bytes = f.read()
            client["queue"].put_nowait(("jpeg", photo_hash_snapshot, jpeg_bytes))
            logging.info("Pushed photo to stream client %s", mac)
        except Exception as e:
            logging.warning("Failed to push photo to %s: %s", mac, e)
