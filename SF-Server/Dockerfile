# syntax=docker/dockerfile:1

FROM python:3.13
ENV TZ=America/New_York
WORKDIR /app
COPY . .
RUN apt update && apt install -y mosquitto nano
RUN pip install watchdog 'paho-mqtt<2.0.0' flask pillow schedule pillow-heif cryptography gunicorn gevent

RUN mkdir -p /app/data

VOLUME ["/app/data"]

CMD ["python3", "/app/syncframe-server.py", "--launch"]

EXPOSE 9369/tcp
EXPOSE 9368
