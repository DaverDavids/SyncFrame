# syntax=docker/dockerfile:1

FROM python:3.13
WORKDIR /app
COPY . .
RUN apt update && apt install -y mosquitto nano
RUN pip install watchdog 'paho-mqtt<2.0.0' flask pillow schedule pillow-heif cryptography gunicorn gevent

RUN mkdir -p /app/data

VOLUME ["/app/data"]

CMD ["gunicorn", \
     "--worker-class", "gevent", \
     "--workers", "1", \
     "--bind", "0.0.0.0:9369", \
     "--timeout", "0", \
     "--keyfile", "/app/data/key.pem", \
     "--certfile", "/app/data/cert.pem", \
     "syncframe-server:app"]
EXPOSE 9369/tcp
EXPOSE 9368
